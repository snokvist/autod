{
  "cap":"pixelpilot",
  "contract_version":"0.1",
  "commands":[
    {"name":"get","description":"Read a Pixel Pilot setting","args":[{"key":"name","type":"enum","control":{"kind":"select","options":["profile","bitrate","stream_url","fec_mode","osd_enabled","latency_mode"],"multi":false},"required":true}]},
    {"name":"set","description":"Set one setting (key=value) and apply instantly","args":[{"key":"pair","type":"string","control":{"kind":"text"},"required":true,"description":"key=value"}]},
    {"name":"params","description":"Set multiple settings and apply instantly","args":[{"key":"pairs","type":"string","control":{"kind":"text"},"required":false,"description":"Repeated key=value tokens"}]},
    {"name":"apply","description":"Re-read settings without changing values (SIGHUP → restart fallback)","args":[]},
    {"name":"start","description":"Start the pixelpilot service","args":[]},
    {"name":"stop","description":"Stop the pixelpilot service","args":[]},
    {"name":"restart","description":"Restart the pixelpilot service","args":[]},
    {"name":"status","description":"Report pixelpilot service state","args":[]},
    {"name":"help","description":"Describe pixelpilot settings and UI hints","args":[]}
  ],
  "settings":[
    {"key":"profile","type":"enum","required":false,"default":"standard","description":"Performance profile preset","control":{"kind":"select","options":["standard","long_range","cinematic"],"multi":false}},
    {"key":"bitrate","type":"int","required":false,"default":16000,"description":"Target bitrate (kbps)","control":{"kind":"range","min":2000,"max":60000,"step":500,"unit":"kbps"}},
    {"key":"stream_url","type":"string","required":false,"default":"udp://224.0.0.1:7700","description":"Primary output socket","control":{"kind":"select","options":["udp://224.0.0.1:7700","udp://224.1.1.1:7700","udp://192.168.0.2:7700","Enter value..."],"multi":false,"allow_free":true}},
    {"key":"fec_mode","type":"enum","required":false,"default":"2/3","description":"Forward error correction ratio","control":{"kind":"select","options":["off","1/2","2/3","3/4","5/6"],"multi":false}},
    {"key":"osd_enabled","type":"bool","required":false,"default":true,"description":"Toggle OSD overlay","control":{"kind":"toggle"}},
    {"key":"latency_mode","type":"enum","required":false,"default":"normal","description":"Latency preset","control":{"kind":"select","options":["low","normal","high"],"multi":false}}
  ]
}
