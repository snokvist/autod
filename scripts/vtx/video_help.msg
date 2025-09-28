{
  "cap": "video",
  "contract_version": "0.2",
  "commands": [
    {"name":"get","description":"Read a single setting","args":[{"key":"name","type":"enum","control":{"kind":"select","options":["codec","fps","bitrate","rcMode","gopSize","size","exposure","mirror","flip","contrast","hue","saturation","luminance","outgoing_server"],"multi":false},"required":true}]},
    {"name":"set","description":"Set one setting (key=value) and apply instantly","args":[{"key":"pair","type":"string","control":{"kind":"text"},"required":true,"description":"key=value"}]},
    {"name":"params","description":"Set multiple settings and apply instantly","args":[{"key":"pairs","type":"string","control":{"kind":"text"},"required":false,"description":"Repeated key=value tokens"}]},
    {"name":"apply","description":"Re-read settings without changing values (SIGHUP → restart fallback)","args":[]},
    {"name":"stop","description":"Stop the streaming service","args":[]},
    {"name":"restart","description":"Restart the streaming service (stop→start)","args":[]},
    {"name":"start","description":"Alias of restart (ensures a clean start)","args":[]},
    {"name":"help","description":"Describe available video settings and UI hints","args":[]}
  ],
  "settings": [
    {"key":"codec","type":"enum","required":false,"default":"h265","description":"Video codec","control":{"kind":"select","options":["h265","h264"],"multi":false}},
    {"key":"fps","type":"enum","required":false,"default":60,"description":"Frames per second","control":{"kind":"select","options":[30,60,90,120],"multi":false}},
    {"key":"bitrate","type":"int","required":false,"default":12544,"description":"Target bitrate (kbps)","control":{"kind":"range","min":2048,"max":20480,"step":512,"unit":"kbps"}},
    {"key":"rcMode","type":"enum","required":false,"default":"cbr","description":"Rate control mode","control":{"kind":"select","options":["cbr","vbr","avbr"],"multi":false}},
    {"key":"gopSize","type":"float","required":false,"default":10,"description":"GOP size (seconds)","control":{"kind":"range","min":0.5,"max":10,"step":0.5,"unit":"s"}},
    {"key":"size","type":"enum","required":false,"default":"1280x720","description":"Frame size","control":{"kind":"select","options":["960x540","1280x720","1920x1080"],"multi":false}},
    {"key":"exposure","type":"int","required":false,"default":7,"description":"ISP exposure","control":{"kind":"range","min":5,"max":32,"step":1}},
    {"key":"mirror","type":"bool","required":false,"default":false,"description":"Horizontal mirror","control":{"kind":"toggle"}},
    {"key":"flip","type":"bool","required":false,"default":false,"description":"Vertical flip","control":{"kind":"toggle"}},
    {"key":"contrast","type":"int","required":false,"default":50,"description":"Image contrast","control":{"kind":"range","min":0,"max":100,"step":1}},
    {"key":"hue","type":"int","required":false,"default":50,"description":"Image hue","control":{"kind":"range","min":0,"max":100,"step":1}},
    {"key":"saturation","type":"int","required":false,"default":50,"description":"Image saturation","control":{"kind":"range","min":0,"max":100,"step":1}},
    {"key":"luminance","type":"int","required":false,"default":50,"description":"Image luminance","control":{"kind":"range","min":0,"max":100,"step":1}},
    {"key":"outgoing_server","type":"string","required":false,"default":"udp://224.0.0.1:5600","description":"Primary output (udp://{ip}:{port})","control":{"kind":"text","placeholder":"udp://192.168.2.20:5600"}}
  ]
}
