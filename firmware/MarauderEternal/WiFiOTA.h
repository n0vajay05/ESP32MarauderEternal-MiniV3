#pragma once

namespace marauder { namespace ota {
bool active();
bool uiActive();
void beginSession();
void endSession();
void run();
void printStatus();
void confirmBoot();
void runChecks();
void runMenuCheck();
void runDownloadCheck();
void runImageChecks(bool reboot);
void runProfileChecks(bool seed);
void runKeypadCheck();
} }
