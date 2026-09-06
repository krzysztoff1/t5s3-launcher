#pragma once
// Serial console. Every line the launcher prints starts with "[launcher]" (or
// "[syscheck]"), and tools/flash.py keys off a few of them:
//   "[launcher] registered"  after `register`
//   "[launcher] booting"     after `boot`
//   "[launcher] menu"        when the menu is on screen
//   "[syscheck] done"        at the end of `syscheck`
namespace console {
void poll();
// True once any byte has arrived - used to cancel the autostart countdown.
bool sawInput();
}
