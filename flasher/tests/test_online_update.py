from __future__ import annotations

import copy
import hashlib
import io
import json
import queue
import tempfile
import threading
import time
import unittest
from pathlib import Path
from unittest.mock import Mock, patch
from urllib.error import URLError
from urllib.request import Request

from flasher import marauder_eternal_flasher as gui
from flasher.online_update import (
    CDN_PREFIX, DOWNLOAD_PREFIX, LATEST_URL, SERVER_ERROR, OnlineFirmware,
    OnlineUpdateError, OnlineUpdater, RepoRedirectHandler, UpdateCancelled,
    UpdateConnectionError, UpdateCheck, allowed_url, version_tuple,
)


def fixture(version='1.16.4'):
    payload = bytearray(1024)
    payload[0] = gui.ESP_IMAGE_MAGIC
    payload[12:14] = gui.ESP32_C5_IMAGE_ID.to_bytes(2, 'little')
    def field(text, size):
        return text.encode().ljust(size, b'\0')
    metadata = (b'MRDRFWID' + bytes((2, 0, 0, 0)) +
                field(gui.EXPECTED_HARDWARE, 48) + field(gui.EXPECTED_CHIP, 12) +
                field('v'+version, 16) + field(gui.EXPECTED_PARTITION_LAYOUT, 32) + b'DIWFRDRM')
    payload[64:192] = metadata
    payload = bytes(payload)
    filename = f'Marauder_Eternal_{version}_MiniV3_ESP32-C5.app.bin'
    digest = hashlib.sha256(payload).hexdigest()
    prefix = DOWNLOAD_PREFIX + 'FirmwareUpdate' + version + '/'
    manifest = {'product': 'ESP32 Marauder Eternal', 'version': version,
                'hardware': 'Marauder Mini V3', 'chip': 'ESP32-C5',
                'flash_size': '8MB', 'psram': True,
                'partition_layout': gui.EXPECTED_PARTITION_LAYOUT,
                'application_flash_offsets': ['0x10000', '0x400000'],
                'images': [{'file': filename, 'offset': '0x10000',
                            'bytes': len(payload), 'sha256': digest}]}
    release = {'tag_name': 'FirmwareUpdate'+version, 'draft': False, 'prerelease': False,
               'assets': [{'name': 'manifest.json', 'state': 'uploaded', 'browser_download_url': prefix+'manifest.json'},
                          {'name': filename, 'state': 'uploaded', 'size': len(payload),
                           'browser_download_url': prefix+filename, 'digest': 'sha256:'+digest}]}
    candidate = OnlineFirmware(version, filename, prefix+filename, len(payload), digest)
    return release, manifest, payload, candidate


class Response(io.BytesIO):
    status = 200


def fake_open(release, manifest, payload):
    def open_response(url, _deadline):
        if url == LATEST_URL:
            return Response(json.dumps(release).encode())
        if url.endswith('/manifest.json'):
            return Response(json.dumps(manifest).encode())
        return Response(payload)
    return open_response


class ReleaseTests(unittest.TestCase):
    def test_numeric_versions_and_invalid_versions(self):
        self.assertGreater(version_tuple('1.16.10'), version_tuple('v1.16.9'))
        for invalid in ('1.16', '1.16.3-rc1', 'FirmwareUpdate1.16.3', '', None):
            with self.subTest(invalid=invalid), self.assertRaises(OnlineUpdateError):
                version_tuple(invalid)

    def test_url_restrictions_and_owned_asset_redirects(self):
        self.assertTrue(allowed_url(LATEST_URL))
        self.assertTrue(allowed_url(DOWNLOAD_PREFIX+'v1.16.4/manifest.json'))
        self.assertTrue(allowed_url('https://release-assets.githubusercontent.com'+CDN_PREFIX+'abc-123?token=abc%2F123', True))
        for url in ('http://github.com/file', 'file:///tmp/update.bin',
                    LATEST_URL+'?page=2', DOWNLOAD_PREFIX+'../manifest.json',
                    DOWNLOAD_PREFIX+'v1.2.3/%2e%2e',
                    'https://github.com.evil.example/'+DOWNLOAD_PREFIX.split('github.com/')[1]+'v1/file',
                    'https://user:pass@github.com/'+DOWNLOAD_PREFIX.split('github.com/')[1]+'v1/file',
                    'https://release-assets.githubusercontent.com/github-production-release-asset/999/abc',
                    'https://github.com:8443/'+DOWNLOAD_PREFIX.split('github.com/')[1]+'v1/file'):
            with self.subTest(url=url):self.assertFalse(allowed_url(url, True))

    def test_redirects_reject_unrelated_hosts_before_following(self):
        with self.assertRaises(OnlineUpdateError):
            RepoRedirectHandler().redirect_request(Request(LATEST_URL), None, 302, '', {}, 'https://example.com/update')

    def test_manifest_and_asset_validation(self):
        release, manifest, payload, candidate = fixture()
        client = OnlineUpdater('1.6.0')
        with patch.object(client, '_open', side_effect=fake_open(release, manifest, payload)):
            self.assertEqual(client.latest(), candidate)
        mutations = [
            lambda r,m: r.update(prerelease=True),
            lambda r,m: r.update(draft=True),
            lambda r,m: r['assets'].append(copy.deepcopy(r['assets'][0])),
            lambda r,m: r['assets'][1].update(browser_download_url='https://example.com/update.bin'),
            lambda r,m: r['assets'][1].update(state='new'),
            lambda r,m: r['assets'][1].update(digest='sha256:'+'0'*64),
            lambda r,m: m.update(chip='ESP32-S3'),
            lambda r,m: m.update(psram=False),
            lambda r,m: m.update(partition_layout='wrong'),
            lambda r,m: m.update(version='1.16.5'),
            lambda r,m: m['images'][0].update(offset='0x0'),
            lambda r,m: m['images'][0].update(bytes=99999999),
            lambda r,m: m['images'].append(copy.deepcopy(m['images'][0])),
        ]
        for index, mutate in enumerate(mutations):
            r,m=copy.deepcopy(release),copy.deepcopy(manifest);mutate(r,m)
            with self.subTest(case=index), patch.object(client,'_open',side_effect=fake_open(r,m,payload)), self.assertRaises(OnlineUpdateError):
                client.latest()

    def test_manifest_digest_is_verified(self):
        release, manifest, payload, _ = fixture()
        release['assets'][0]['digest']='sha256:'+'0'*64
        client=OnlineUpdater('1.6.0')
        with patch.object(client,'_open',side_effect=fake_open(release,manifest,payload)),self.assertRaisesRegex(OnlineUpdateError,'manifest checksum'):
            client.latest()

    def test_same_and_older_releases_do_not_offer_downgrades(self):
        client=OnlineUpdater('1.6.0');candidate=fixture()[3]
        with patch.object(client,'latest',return_value=candidate):
            self.assertIsNone(client.check('1.16.4'))
            self.assertIsNone(client.check('1.16.10'))
            self.assertEqual(client.check('1.16.3'),candidate)

    def test_connection_errors_and_deadlines(self):
        client=OnlineUpdater('1.6.0')
        with patch.object(client.opener,'open',side_effect=URLError('offline')):
            with self.assertRaisesRegex(UpdateConnectionError,SERVER_ERROR):
                client._open(LATEST_URL,time.monotonic()+10)
        with self.assertRaises(UpdateConnectionError):client._open(LATEST_URL,time.monotonic()-1)

    def test_json_is_bounded_and_html_is_rejected(self):
        client=OnlineUpdater('1.6.0')
        for payload in (b'<html>not a release</html>', b' '*1025):
            with patch.object(client,'_open',return_value=Response(payload)),self.assertRaises(OnlineUpdateError):
                client._json(LATEST_URL,time.monotonic()+5,1024)


class DownloadTests(unittest.TestCase):
    def test_verified_download_commits_and_reports_progress(self):
        _,_,payload,candidate=fixture();client=OnlineUpdater('1.6.0');progress=[]
        with tempfile.TemporaryDirectory() as directory:
            path=Path(directory)/'update.bin';path.write_bytes(b'old firmware')
            with patch.object(client,'_open',return_value=Response(payload)):
                client.download(candidate,path,gui.validate_online_firmware,progress.append)
            self.assertEqual(path.read_bytes(),payload)
            self.assertEqual(progress[-1],100)
            self.assertEqual(list(Path(directory).iterdir()),[path])

    def test_failed_download_never_replaces_existing_file(self):
        _,_,payload,candidate=fixture()
        for damaged in (payload[:-1],payload+b'extra',b'X'+payload[1:]):
            client=OnlineUpdater('1.6.0')
            with self.subTest(length=len(damaged)),tempfile.TemporaryDirectory() as directory:
                path=Path(directory)/'update.bin';path.write_bytes(b'old firmware')
                with patch.object(client,'_open',return_value=Response(damaged)),self.assertRaises(OnlineUpdateError):
                    client.download(candidate,path,gui.validate_online_firmware)
                self.assertEqual(path.read_bytes(),b'old firmware')
                self.assertEqual(list(Path(directory).iterdir()),[path])

    def test_valid_checksum_with_wrong_embedded_version_is_rejected(self):
        _,_,payload,_=fixture('1.16.3');original=fixture()[3]
        candidate=OnlineFirmware(original.version,original.filename,original.url,len(payload),hashlib.sha256(payload).hexdigest())
        client=OnlineUpdater('1.6.0')
        with tempfile.TemporaryDirectory() as directory:
            path=Path(directory)/'update.bin'
            with patch.object(client,'_open',return_value=Response(payload)),self.assertRaisesRegex(OnlineUpdateError,'identity'):
                client.download(candidate,path,gui.validate_online_firmware)
            self.assertEqual(list(Path(directory).iterdir()),[])

    def test_cancellation_cleans_partial_file(self):
        _,_,payload,candidate=fixture();cancel=threading.Event();client=OnlineUpdater('1.6.0',cancel)
        with tempfile.TemporaryDirectory() as directory:
            with patch.object(client,'_open',return_value=Response(payload)),self.assertRaises(UpdateCancelled):
                client.download(candidate,Path(directory)/'update.bin',gui.validate_online_firmware,lambda _:cancel.set())
            self.assertEqual(list(Path(directory).iterdir()),[])


def bare_app():
    app=object.__new__(gui.FlasherApp)
    app.root=Mock();app.flashing=False;app.sd_busy=False;app.online_busy=False
    app.sd_window=None;app.mirror_window=None;app.closed=False
    app.startup_update_pending=True;app.quit_after_online=False;app.pending_updates=None
    app.online_cancel=threading.Event();app.events=queue.Queue()
    app.firmware=None;app.firmware_stamp=None;app.ports=[];app.raw_output=[];app.included_available=False
    for name in ('status_var','status_label','firmware_path_var','firmware_detail_var',
                 'firmware_warning_var','online_button','included_button','progress'):
        setattr(app,name,Mock())
    app._set_main_controls_locked=Mock();app.refresh_ports=Mock();app._append_log=Mock()
    return app


class UpdateUiTests(unittest.TestCase):
    def test_missing_manifest_has_download_bootstrap_metadata(self):
        with patch.object(gui,'release_manifest_path',return_value=Path('/does/not/exist/manifest.json')):
            self.assertEqual(gui.load_release_manifest()['version'],gui.FIRMWARE_VERSION)

    def test_missing_bin_uses_download_label_and_has_no_baseline(self):
        app=bare_app();app._refresh_firmware_availability()
        app.online_button.configure.assert_called_with(text='Download Firmware')
        self.assertIsNone(app._current_update_version())
        candidate=fixture('1.16.3')[3]
        with patch.object(gui,'OnlineUpdater') as updater:
            updater.return_value.check_all.return_value=UpdateCheck(firmware=candidate)
            app._online_check_worker(None)
            updater.return_value.check_all.assert_called_once_with(None,gui.APP_VERSION)
        self.assertEqual(app.events.get_nowait(),('online_checked_all',UpdateCheck(firmware=candidate)))

    def test_selected_image_and_deleted_image_change_button_and_baseline(self):
        _,_,payload,_=fixture('1.16.4');app=bare_app()
        with tempfile.TemporaryDirectory() as directory:
            path=Path(directory)/'firmware.bin';path.write_bytes(payload)
            app._select_firmware(gui.inspect_firmware(path),str(path))
            app.online_button.configure.assert_called_with(text='Check Online For Update')
            self.assertEqual(version_tuple(app._current_update_version()),version_tuple('1.16.4'))
            path.unlink();app._refresh_firmware_availability()
            self.assertIsNone(app.firmware)
            app.online_button.configure.assert_called_with(text='Download Firmware')

    def test_corrupted_selected_bin_becomes_unavailable(self):
        _,_,payload,_=fixture();app=bare_app()
        with tempfile.TemporaryDirectory() as directory:
            path=Path(directory)/'firmware.bin';path.write_bytes(payload)
            app._select_firmware(gui.inspect_firmware(path),str(path))
            path.write_bytes(b'broken')
            app._refresh_firmware_availability()
            self.assertIsNone(app.firmware)
            app.online_button.configure.assert_called_with(text='Download Firmware')

    def test_inaccessible_included_directory_does_not_break_recovery(self):
        app=bare_app();inaccessible=Mock();inaccessible.is_file.side_effect=PermissionError('unreadable')
        with patch.object(gui,'resource_path',return_value=inaccessible):app._refresh_firmware_availability()
        self.assertIsNone(app.firmware)
        app.online_button.configure.assert_called_with(text='Download Firmware')

    def test_startup_defers_for_usb_operations_and_manual_check_prevents_duplicate(self):
        app=bare_app();app.flashing=True;app.check_online_for_update=Mock()
        app._startup_online_check();app.check_online_for_update.assert_not_called()
        app.root.after.assert_called_once()
        app.flashing=False;app._startup_online_check();app.check_online_for_update.assert_called_once()
        app.startup_update_pending=False;app._startup_online_check()
        app.check_online_for_update.assert_called_once()

    def test_check_runs_on_worker_and_keeps_controls_locked(self):
        app=bare_app()
        with patch.object(gui.threading,'Thread') as worker:
            app.check_online_for_update()
            worker.return_value.start.assert_called_once()
            self.assertEqual(worker.call_args.kwargs['args'],(None,))
        self.assertTrue(app.online_busy);self.assertFalse(app.startup_update_pending)
        app._set_main_controls_locked.assert_called_with(True)

    def test_exact_messages_and_declining_preserves_selection(self):
        app=bare_app();app.online_busy=True
        with patch.object(gui.messagebox,'showinfo') as info:
            app._handle_online_event('online_checked',None)
            self.assertEqual(info.call_args.args[1],'Up To Date')
        candidate=fixture()[3];app.online_busy=True
        with patch.object(gui.messagebox,'askyesno',return_value=False) as prompt:
            app._handle_online_event('online_checked',candidate)
            self.assertEqual(prompt.call_args.args[1],'Newest MarauderEternal version 1.16.4 found, do you want to download?')
        self.assertIsNone(app.firmware);self.assertFalse(app.online_busy)
        with patch.object(gui.messagebox,'showerror') as error:
            app._handle_online_event('online_error',(True,URLError('offline')))
            self.assertEqual(error.call_args.args[1],SERVER_ERROR)

    def test_close_cancels_worker_before_destroying_root(self):
        app=bare_app();app.online_busy=True
        app._on_close();self.assertTrue(app.online_cancel.is_set())
        app.root.destroy.assert_not_called()
        self.assertFalse(app._handle_online_event('online_error',(True,UpdateCancelled())))
        app.root.destroy.assert_called_once()

    def test_no_bin_never_starts_flash(self):
        app=bare_app()
        with patch.object(gui.messagebox,'showerror'),patch.object(gui,'invoke_esptool') as flash:
            app.start_flash();flash.assert_not_called()

