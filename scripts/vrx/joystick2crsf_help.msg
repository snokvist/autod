{
  "cap":"joystick2crsf",
  "contract_version":"0.1",
  "config_path":"/etc/joystick2crsf.conf",
  "commands":[
    {"name":"get","description":"Read a joystick2crsf setting","args":[{"key":"name","type":"string","required":true}]},
    {"name":"set","description":"Update a joystick2crsf setting","args":[{"key":"pair","type":"string","required":true,"description":"key=value or key value"}]},
    {"name":"reload","description":"Send SIGHUP so joystick2crsf reloads its config","args":[]},
    {"name":"start","description":"Start the joystick2crsf service","args":[]},
    {"name":"stop","description":"Stop the joystick2crsf service","args":[]},
    {"name":"restart","description":"Restart the joystick2crsf service","args":[]},
    {"name":"status","description":"Show joystick2crsf service status","args":[]},
    {"name":"help","description":"Describe joystick2crsf controls","args":[]}
  ],
  "settings":[
    {"key":"rate","type":"int","default":125,"description":"CRSF frame rate (Hz)","control":{"kind":"select","options":[50,125,250],"multi":false}},
    {"key":"channels","type":"bool","default":false,"description":"Log CRSF channels each frame","control":{"kind":"toggle"}},
    {"key":"stats","type":"bool","default":false,"description":"Log timing and telemetry stats","control":{"kind":"toggle"}},
    {"key":"joystick_index","type":"int","default":0,"description":"SDL joystick index (0 = first)","control":{"kind":"range","min":0,"max":8,"step":1}},
    {"key":"rescan_interval","type":"int","default":5,"description":"Seconds between reconnect attempts","control":{"kind":"range","min":1,"max":60,"step":1,"unit":"s"}},
    {"key":"map","type":"string","default":"1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16","description":"Channel map (CSV of joystick axes → CRSF)","control":{"kind":"text","placeholder":"1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16"}},
    {"key":"invert","type":"string","default":"0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0","description":"Channel inversion flags (0/1 CSV)","control":{"kind":"text","placeholder":"0,0,0,…"}},
    {"key":"deadband","type":"string","default":"0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0","description":"Deadband per channel (CSV, raw units)","control":{"kind":"text","placeholder":"0,0,0,…"}},
    {"key":"arm_toggle","type":"int","default":5,"description":"Momentary index that latches channel 5 high (0 disables)","control":{"kind":"range","min":0,"max":16,"step":1}},
    {"key":"udp_enabled","type":"bool","default":true,"description":"Enable UDP CRSF bridge","control":{"kind":"toggle"}},
    {"key":"udp_target","type":"string","default":"192.168.0.1:14550","description":"UDP host:port target","control":{"kind":"text","placeholder":"192.168.0.1:14550"}},
    {"key":"sse_enabled","type":"bool","default":false,"description":"Expose SSE telemetry stream","control":{"kind":"toggle"}},
    {"key":"sse_bind","type":"string","default":"127.0.0.1:8070","description":"SSE bind host:port","control":{"kind":"text","placeholder":"*:8070"}},
    {"key":"sse_path","type":"string","default":"/sse","description":"HTTP path for the SSE feed","control":{"kind":"text","placeholder":"/sse"}}
  ],
  "notes":[
    "Boolean fields accept 1/0, true/false, yes/no.",
    "Use reload after updating values to apply changes without restarting."
  ]
}
