#include "TrackerDiagnostics.h"
#include "FindMyScanTimer.h"
#include <cassert>
#include <cstring>
#include <iostream>
int main() {
  using namespace marauder::tracker;
  ScanTimer timer;
  assert(!timer.expired(60000));
  timer.start(100);
  assert(!timer.expired(60099) && timer.expired(60100));
  timer.cancel();
  assert(!timer.expired(60101));
  timer.start(200000);
  assert(!timer.expired(259999) && timer.expired(260000));
  timer.start(UINT32_MAX - 29999);
  assert(!timer.expired(29999) && timer.expired(30000));
  timer.start(0);
  assert(!timer.expired(59999) && timer.expired(60000));
  unsigned sends = 0, cleanups = 0;
  Report r;
  auto inspect = [&]() { r.linkEstablished = true; r.services = 4; r.characteristics = 2; return 0; };
  auto send = [&](int p) { assert(p == 0); ++sends; return true; };
  auto cleanup = [&]() { ++cleanups; return true; };
  assert(run(r,true,inspect,send,cleanup));
  assert(sends == 0 && cleanups == 1 && r.result == Result::Ready && r.stage == Stage::Ready);
  assert(run(r,false,inspect,send,cleanup));
  assert(sends == 1 && cleanups == 2 && r.result == Result::SoundSent);
  for (auto result : {Result::ClientFailed,Result::ConnectFailed,Result::NoServices,Result::NoSoundService,Result::NoCharacteristic}) {
    r = Report(); r.result = result; r.stage = Stage::Connect; r.error = 13;
    assert(!run(r,true,[](){ return -1; },send,cleanup));
    assert(r.result == result && r.error == 13 && sends == 1);
  }
  r = Report();
  assert(!run(r,false,inspect,[](int){return false;},[](){return false;}));
  assert(r.result == Result::SoundFailed && r.cleanupPending);
  assert(!strcmp(errorName(13),"Operation timeout"));
  assert(!strcmp(errorName(0x23e),"Link not established"));
  assert(!strcmp(errorName(9999),"See raw error code"));
  assert(strcmp(resultName(Result::ConnectFailed),resultName(Result::NoServices)));
  std::cout << "FindMy timer: 60-second boundary, cancellation, fresh restart and clock rollover passed\n";
  std::cout << "Tracker diagnostics: test-only never writes, failure stages/errors survive cleanup, send and deferred cleanup paths passed\n";
}
