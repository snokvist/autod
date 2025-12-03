{
  "cap": "fw",
  "contract_version": "0.2",
  "help_endpoints": ["/sys/fw/help"],
  "commands": [
    {"name":"help","description":"Describe firmware environment controls","args":[]},
    {"name":"get","description":"Read one firmware env value","args":[{"key":"name","type":"enum","control":{"kind":"select","options":["link_mode","wlanssid","msposd_tty","wlanpass"],"multi":false,"allow_free":true},"required":true}]},
    {"name":"set","description":"Set a firmware env key=value pair","args":[{"key":"pair","type":"string","control":{"kind":"text"},"required":true,"description":"key=value"}]},
    {"name":"params","description":"Set multiple firmware env pairs","args":[{"key":"pairs","type":"string","control":{"kind":"text"},"required":false,"description":"Repeated key=value tokens"}]},
    {"name":"ota_force","description":"Force OTA upgrade via sysupgrade (SSE stream on port 6666)","args":[{"key":"url","type":"string","control":{"kind":"text","placeholder":"http://update.bin"},"required":true}]}
  ],
  "settings": [
    {"key":"link_mode","type":"enum","description":"Firmware link mode (apfpv, wfb, or empty to clear)","control":{"kind":"select","options":["apfpv","wfb",""],"multi":false,"allow_free":true}},
    {"key":"wlanssid","type":"string","description":"Wi-Fi SSID (4-24 alphanumerics or empty to clear)","control":{"kind":"text","placeholder":"wlanssid"}},
    {"key":"msposd_tty","type":"enum","description":"MSP/OSD serial device","control":{"kind":"select","options":["standalone","/dev/ttyS1","/dev/ttyS2"],"multi":false}},
    {"key":"wlanpass","type":"string","description":"Wi-Fi passphrase (8-24 alphanumerics or empty to clear)","control":{"kind":"text","placeholder":"wlanpass"}}
  ]
}
