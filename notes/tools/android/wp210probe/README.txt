wp210 probe kit (read-only) - stock Android with Magisk root

Runs by itself at every Android boot (Magisk post-fs-data.d + service.d):
 - from the earliest moment: records every MediaTek secure call (SIP SMC) -> smc.txt
 - key DVFSRC/SPM registers + DRAM rate: every 2 s during boot, every 5 s for 3 min,
   then every 30 s, and on every screen change / resume -> summary.txt
 - full dumps (driver dump, DVFSRC page, 416 SPM registers): first, booted, 60 s, 180 s,
   20 s after screen off (idle), after every resume, end -> full-<uptime>-<tag>.txt
 - events.txt: what happened when
 - stops by itself after 3600 s (install.sh <seconds> to change)
Results: /data/local/tmp/wp210-probe/<boot id>/   (one folder per boot)

Install (cmd, in wp210-shim, folder wp210probe next to adb):
  adb push wp210probe /data/local/tmp/ && adb shell su -c "sh /data/local/tmp/wp210probe/install.sh"
Check it works now (no reboot):
  adb shell su -c "sh /data/adb/wp210/now.sh"
Collect:
  adb shell su -c "sh /data/adb/wp210/collect.sh" && adb pull /data/local/tmp/wp210-probe.tgz
Stop / remove:
  adb shell su -c "sh /data/adb/wp210/stop.sh"
  adb shell su -c "sh /data/adb/wp210/uninstall.sh"
