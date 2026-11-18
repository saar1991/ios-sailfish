#!/system/bin/sh
# wp210 probe kit: pack every boot's results into /data/local/tmp/wp210-probe.tgz
cd /data/local/tmp || exit 1
rm -f wp210-probe.tgz
tar czf wp210-probe.tgz wp210-probe && chmod 644 wp210-probe.tgz
ls -la wp210-probe.tgz; du -sh wp210-probe; ls wp210-probe
