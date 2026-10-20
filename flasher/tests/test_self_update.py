from __future__ import annotations

import copy
import hashlib
import importlib.util
import json
import os
from pathlib import Path
import tempfile
import time
import unittest
from unittest.mock import Mock, patch

from flasher import marauder_eternal_flasher as gui
from flasher import self_update as install
from flasher.online_update import (
    DOWNLOAD_PREFIX, RELEASES_URL, OnlineFlasher, OnlineUpdater, UpdateCheck,
    allowed_url, current_platform, SERVER_ERROR, UpdateConnectionError,
)
from flasher.tests.test_online_update import Response, bare_app, fixture


def executable(platform='linux-x86_64'):
    payload=bytearray(1024)
    if platform.startswith('linux'):
        payload[:7]=b'\x7fELF\x02\x01\x01';payload[16:18]=(2).to_bytes(2,'little');payload[18:20]=(62).to_bytes(2,'little')
    else:
        payload[:2]=b'MZ';payload[60:64]=(128).to_bytes(4,'little')
        payload[128:134]=b'PE\0\0\x64\x86';payload[150:152]=(2).to_bytes(2,'little')
    return bytes(payload)


def desktop_fixture(version='1.8.0', platform='linux-x86_64'):
    payload=executable(platform)
    extension='.exe' if platform.startswith('windows') else ''
    name=f'MarauderEternalFlasher-{version}-{platform}{extension}'
    prefix=DOWNLOAD_PREFIX+'FlasherUpdate'+version+'/'
    digest=hashlib.sha256(payload).hexdigest()
    metadata={'product':'Marauder Eternal Flasher','schema':1,'update_protocol':1,
              'version':version,'platform':platform,'file':name,'bytes':len(payload),'sha256':digest}
    release={'tag_name':'FlasherUpdate'+version,'draft':False,'prerelease':False,
             'assets':[{'name':name,'state':'uploaded','browser_download_url':prefix+name,'size':len(payload),'digest':'sha256:'+digest},
                       {'name':f'flasher-{platform}.json','state':'uploaded','browser_download_url':prefix+f'flasher-{platform}.json'}]}
    candidate=OnlineFlasher(version,name,prefix+name,len(payload),digest,platform)
    return release,metadata,payload,candidate


def release_server(firmware_version='1.16.4', flasher_version='1.8.0'):
    fw,fwm,fwb,fw_candidate=fixture(firmware_version)
    app,appm,appb,app_candidate=desktop_fixture(flasher_version)
    routes={RELEASES_URL:[app,fw],fw['assets'][0]['browser_download_url']:fwm,
            app['assets'][1]['browser_download_url']:appm}
    def open_response(url,_deadline):
        value=routes[url]
        return Response(json.dumps(value).encode())
    return routes,open_response,fw_candidate,app_candidate


class CombinedReleaseTests(unittest.TestCase):
    def test_combined_check_handles_independent_app_and_firmware_releases(self):
        _,server,firmware,flasher=release_server();client=OnlineUpdater(gui.APP_VERSION)
        with patch('flasher.online_update.current_platform',return_value='linux-x86_64'),patch.object(client,'_open',side_effect=server) as requests:
            result=client.check_all('1.16.3','1.7.0')
        self.assertEqual(result,UpdateCheck(firmware,flasher))
        self.assertEqual(sum(call.args[0]==RELEASES_URL for call in requests.call_args_list),1)

    def test_numeric_versions_and_prerelease_filter(self):
        routes,server,_,_=release_server('1.16.10','1.7.10')
        older,_,_,_=desktop_fixture('1.7.9')
        prerelease,_,_,_=desktop_fixture('9.0.0');prerelease['prerelease']=True
        routes[RELEASES_URL]=[prerelease,older,*routes[RELEASES_URL]]
        client=OnlineUpdater(gui.APP_VERSION)
        with patch('flasher.online_update.current_platform',return_value='linux-x86_64'),patch.object(client,'_open',side_effect=server):
            result=client.check_all('1.16.9','1.7.9')
        self.assertEqual(result.flasher.version,'1.7.10')
        self.assertEqual(result.firmware.version,'1.16.10')

    def test_no_flasher_downgrades_and_missing_bin_still_offers_firmware(self):
        _,server,firmware,_=release_server('1.16.3','1.6.0');client=OnlineUpdater(gui.APP_VERSION)
        with patch('flasher.online_update.current_platform',return_value='linux-x86_64'),patch.object(client,'_open',side_effect=server):
            result=client.check_all(None,'1.7.0')
        self.assertEqual(result.firmware,firmware);self.assertIsNone(result.flasher)

    def test_old_releases_without_flasher_assets_keep_firmware_updates(self):
        routes,server,firmware,_=release_server();routes[RELEASES_URL]=routes[RELEASES_URL][1:]
        client=OnlineUpdater(gui.APP_VERSION)
        with patch.object(client,'_open',side_effect=server):result=client.check_all('1.16.3','1.7.0')
        self.assertEqual(result.firmware,firmware);self.assertIsNone(result.flasher_error)

    def test_bad_flasher_metadata_does_not_block_firmware_updates(self):
        for change in ({'platform':'windows-x86_64'},{'sha256':'0'*64},{'version':'99.0.0'},
                       {'update_protocol':2},{'file':'evil.exe'},{'bytes':True}):
            routes,server,firmware,_=release_server()
            for key,value in routes.items():
                if key.endswith('/flasher-linux-x86_64.json'):value.update(change)
            client=OnlineUpdater(gui.APP_VERSION)
            with self.subTest(change=change),patch('flasher.online_update.current_platform',return_value='linux-x86_64'),patch.object(client,'_open',side_effect=server):
                result=client.check_all('1.16.3','1.7.0')
            self.assertEqual(result.firmware,firmware);self.assertIsNone(result.flasher)
            self.assertIsNotNone(result.flasher_error)

    def test_supported_platform_and_url_scope(self):
        self.assertTrue(allowed_url(RELEASES_URL))
        self.assertFalse(allowed_url(RELEASES_URL+'&page=2'))
        with patch('flasher.online_update.platform.machine',return_value='ARM64'):
            self.assertIsNone(current_platform())

    def test_flasher_connection_error_uses_requested_server_message(self):
        _,server,firmware,_=release_server();client=OnlineUpdater(gui.APP_VERSION)
        def unavailable(url,deadline):
            if url.endswith('/flasher-linux-x86_64.json'):raise UpdateConnectionError(SERVER_ERROR)
            return server(url,deadline)
        with patch('flasher.online_update.current_platform',return_value='linux-x86_64'),patch.object(client,'_open',side_effect=unavailable):
            result=client.check_all('1.16.3','1.7.0')
        self.assertEqual(result.firmware,firmware);self.assertEqual(result.flasher_error,SERVER_ERROR)

    def test_flasher_download_uses_same_atomic_integrity_checks(self):
        _,_,payload,candidate=desktop_fixture();client=OnlineUpdater(gui.APP_VERSION)
        with tempfile.TemporaryDirectory() as directory:
            path=Path(directory)/'download'
            with patch.object(client,'_open',return_value=Response(payload)):
                client.download(candidate,path,lambda path,version:install.validate_executable(path,candidate.platform))
            self.assertEqual(path.read_bytes(),payload)


class InstallerTests(unittest.TestCase):
    def make_job(self, root, exists=True):
        target=root/'Flasher with spaces';old=executable()+b'previous'
        if exists:target.write_bytes(old)
        stage=install.create_stage(target);source=stage/'download';source.write_bytes(executable())
        job={'schema':1,'parent_pid':123,'source':str(source),'target':str(target),
             'old_sha256':install.file_hash(target) if exists else None,
             'sha256':install.file_hash(source),'version':'1.8.0','platform':'linux-x86_64',
             'firmware':str(root/'firmware with spaces.app.bin')}
        job_path=stage/'job.json';job_path.write_text(json.dumps(job))
        return target,old,source,job_path

    def test_native_header_checks_reject_wrong_os_and_cpu(self):
        with tempfile.TemporaryDirectory() as directory:
            path=Path(directory)/'app'
            for platform in ('linux-x86_64','windows-x86_64'):
                path.write_bytes(executable(platform));install.validate_executable(path,platform)
                with self.assertRaises(ValueError):install.validate_executable(path,'windows-x86_64' if platform.startswith('linux') else 'linux-x86_64')
            path.write_bytes(b'not an executable')
            with self.assertRaises(ValueError):install.validate_executable(path,'linux-x86_64')

    def test_health_report_checks_version_without_console_output(self):
        with tempfile.TemporaryDirectory() as directory:
            path=Path(directory)/'app';path.write_bytes(executable())
            for version in ('1.8.0','1.7.0'):
                def run(command,**kwargs):
                    Path(command[2]).write_text(json.dumps({'product':gui.APP_NAME,'version':version,'platform':'linux-x86_64','update_protocol':1,'status':'ok'}))
                    return Mock(returncode=0)
                with patch.object(install.subprocess,'run',side_effect=run):
                    if version=='1.8.0':install.verify_flasher(path,'1.8.0','linux-x86_64')
                    else:
                        with self.assertRaisesRegex(ValueError,'version/startup'):install.verify_flasher(path,'1.8.0','linux-x86_64')

    def test_wait_replace_backup_and_restart_preserve_firmware_argument(self):
        with tempfile.TemporaryDirectory() as directory:
            target,old,source,job=self.make_job(Path(directory));replacement=source.read_bytes()
            def waited(pid):
                self.assertEqual(pid,123);self.assertEqual(target.read_bytes(),old)
            with patch.object(install,'wait_for_parent',side_effect=waited),patch.object(install.subprocess,'Popen') as launch:
                install.apply_update(job)
            self.assertEqual(target.read_bytes(),replacement)
            self.assertEqual(target.with_name(target.name+'.previous').read_bytes(),old)
            self.assertEqual(launch.call_args.args[0],[str(target),'--firmware',str(Path(directory)/'firmware with spaces.app.bin')])
            self.assertEqual(launch.call_args.kwargs['env']['PYINSTALLER_RESET_ENVIRONMENT'],'1')

    def test_failed_restart_restores_previous_executable(self):
        with tempfile.TemporaryDirectory() as directory:
            target,old,_,job=self.make_job(Path(directory))
            with patch.object(install,'wait_for_parent'),patch.object(install.subprocess,'Popen',side_effect=OSError('launch failed')),self.assertRaises(OSError):
                install.apply_update(job)
            self.assertEqual(target.read_bytes(),old)

    def test_tampered_download_or_changed_target_does_not_replace(self):
        for tamper in ('source','target'):
            with self.subTest(tamper=tamper),tempfile.TemporaryDirectory() as directory:
                target,old,source,job=self.make_job(Path(directory))
                path=source if tamper=='source' else target;path.write_bytes(b'changed')
                expected=target.read_bytes()
                with patch.object(install,'wait_for_parent'),self.assertRaises(ValueError):install.apply_update(job)
                self.assertEqual(target.read_bytes(),expected)

    def test_parent_timeout_never_changes_current_executable(self):
        with tempfile.TemporaryDirectory() as directory:
            target,old,_,job=self.make_job(Path(directory))
            with patch.object(install,'wait_for_parent',side_effect=TimeoutError()),self.assertRaises(TimeoutError):install.apply_update(job)
            self.assertEqual(target.read_bytes(),old)

    def test_frozen_handoff_copies_helper_and_uses_independent_process(self):
        with tempfile.TemporaryDirectory() as directory:
            target,_,source,job=self.make_job(Path(directory))
            with patch.object(install.sys,'frozen',True,create=True),patch.object(install.sys,'executable',str(target)),patch.object(install.subprocess,'Popen') as launch:
                install.prepare_handoff(job.parent,target,'1.8.0',install.file_hash(source),'linux-x86_64')
            arguments=launch.call_args.args[0]
            self.assertNotEqual(arguments[0],str(target));self.assertTrue(Path(arguments[0]).is_file())
            self.assertEqual(arguments[1],'--apply-flasher-update')
            self.assertEqual(launch.call_args.kwargs['env']['PYINSTALLER_RESET_ENVIRONMENT'],'1')

    def test_library_environment_is_reset_for_independent_bootloader(self):
        with patch.dict(os.environ,{'LD_LIBRARY_PATH':'/old/MEI','LD_LIBRARY_PATH_ORIG':'/system/lib'}):
            self.assertEqual(install.independent_environment()['LD_LIBRARY_PATH'],'/system/lib')

    def test_locked_windows_replace_is_retried(self):
        with patch.object(install.os,'replace',side_effect=[PermissionError(),None]) as replace,patch.object(install.time,'sleep'):
            install._replace_with_retry(Path('new'),Path('old'))
        self.assertEqual(replace.call_count,2)

    def test_successful_source_install_has_no_existing_target(self):
        with tempfile.TemporaryDirectory() as directory:
            target,_,source,job=self.make_job(Path(directory),exists=False);expected=source.read_bytes()
            with patch.object(install,'wait_for_parent'),patch.object(install.subprocess,'Popen'):install.apply_update(job)
            self.assertEqual(target.read_bytes(),expected)

    def test_completed_cleanup_preserves_unrelated_directories(self):
        with tempfile.TemporaryDirectory() as directory:
            target,_,_,job=self.make_job(Path(directory));(job.parent/'completed').write_text('1')
            other=install.create_stage(target);(other/'job.json').write_text('{}')
            install.cleanup_completed(target)
            self.assertFalse(job.parent.exists());self.assertTrue(other.exists())


class FlasherFlowTests(unittest.TestCase):
    def test_health_cli_works_with_windowed_stdout_and_stderr(self):
        with tempfile.TemporaryDirectory() as directory:
            report=Path(directory)/'health.json'
            with patch.object(gui.sys,'stdout',None),patch.object(gui.sys,'stderr',None),patch.object(gui,'discover_ports',return_value=[]):
                self.assertEqual(gui.main(['--update-health',str(report)]),0)
            self.assertEqual(json.loads(report.read_text())['version'],gui.APP_VERSION)

    def test_declining_flasher_still_offers_firmware_and_never_reports_up_to_date(self):
        app=bare_app();firmware=fixture()[3];flasher=desktop_fixture()[3]
        with patch.object(gui.messagebox,'askyesno',return_value=False) as ask,patch.object(gui.messagebox,'showinfo') as info:
            app._handle_online_event('online_checked_all',UpdateCheck(firmware,flasher))
        self.assertEqual(ask.call_count,2);info.assert_not_called()
        self.assertIn('Flasher version 1.8.0',ask.call_args_list[0].args[1])
        self.assertIn('version 1.16.4',ask.call_args_list[1].args[1])

    def test_flasher_error_does_not_hide_firmware_offer(self):
        app=bare_app()
        with patch.object(gui.messagebox,'showerror') as error,patch.object(gui.messagebox,'askyesno',return_value=False) as ask:
            app._handle_online_event('online_checked_all',UpdateCheck(firmware=fixture()[3],flasher_error='package unavailable'))
        error.assert_called_once();ask.assert_called_once()

    def test_successful_handoff_closes_only_after_helper_starts(self):
        app=bare_app();app.online_busy=True
        with tempfile.TemporaryDirectory() as directory,patch.object(gui,'prepare_handoff') as helper:
            stage=Path(directory);result=app._handle_online_event('online_flasher_ready',(desktop_fixture()[3],stage/'Flasher',stage))
            helper.assert_called_once();app.root.destroy.assert_called_once()
        self.assertFalse(result);self.assertTrue(app.closed)

    def test_handoff_failure_keeps_launcher_open_and_continues_firmware(self):
        app=bare_app();app.pending_updates=UpdateCheck(firmware=fixture()[3])
        with tempfile.TemporaryDirectory() as directory,patch.object(gui,'prepare_handoff',side_effect=OSError('cannot start helper')),patch.object(gui.messagebox,'showerror'),patch.object(gui.messagebox,'askyesno',return_value=False) as ask:
            stage=Path(directory)/'stage';stage.mkdir()
            app._handle_online_event('online_flasher_ready',(desktop_fixture()[3],Path(directory)/'Flasher',stage))
            self.assertFalse(stage.exists());ask.assert_called_once()
        app.root.destroy.assert_not_called()


class ReleasePackagingTests(unittest.TestCase):
    def test_package_metadata_agrees_with_executable_and_digests(self):
        spec=importlib.util.spec_from_file_location('package_flasher',Path(__file__).resolve().parents[2]/'scripts/package_flasher.py')
        module=importlib.util.module_from_spec(spec);spec.loader.exec_module(module)
        with tempfile.TemporaryDirectory() as directory:
            path=Path(directory)/'Flasher';path.write_bytes(executable())
            with patch.object(module,'verify_flasher'):
                package,metadata,checksums=module.package_flasher(path,'1.8.0','linux-x86_64')
            info=json.loads(metadata.read_text())
            self.assertEqual(info['sha256'],install.file_hash(package))
            self.assertEqual(info['bytes'],package.stat().st_size)
            self.assertEqual(info['file'],package.name)
            self.assertEqual(info['update_protocol'],1)
            self.assertIn(install.file_hash(metadata),checksums.read_text())

