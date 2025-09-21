{
  "cap":"udp_relay",
  "contract_version":"0.1",
  "commands":[
    {"name":"get","description":"Read a UDP relay parameter","args":[{"key":"name","type":"enum","control":{"kind":"select","options":["listen_addr","listen_port","target_addr","target_port","ttl","max_bitrate","enabled"],"multi":false},"required":true}]},
    {"name":"set","description":"Set one UDP relay parameter (key=value)","args":[{"key":"pair","type":"string","control":{"kind":"text"},"required":true}]},
    {"name":"params","description":"Set multiple UDP relay parameters","args":[{"key":"pairs","type":"string","control":{"kind":"text"},"required":false}]},
    {"name":"start","description":"Start the UDP relay service","args":[]},
    {"name":"stop","description":"Stop the UDP relay service","args":[]},
    {"name":"status","description":"UDP relay service status","args":[]},
    {"name":"help","description":"Describe UDP relay controls","args":[]}
  ],
  "settings":[
    {"key":"listen_addr","type":"string","required":false,"default":"0.0.0.0","description":"Local bind address","control":{"kind":"text"}},
    {"key":"listen_port","type":"int","required":false,"default":5700,"description":"Local bind port","control":{"kind":"range","min":1024,"max":65535,"step":1}},
    {"key":"target_addr","type":"string","required":false,"default":"224.0.0.1","description":"Downlink multicast/unicast address","control":{"kind":"text"}},
    {"key":"target_port","type":"int","required":false,"default":7700,"description":"Downlink port","control":{"kind":"range","min":1024,"max":65535,"step":1}},
    {"key":"ttl","type":"int","required":false,"default":8,"description":"Outgoing TTL","control":{"kind":"range","min":1,"max":32,"step":1}},
    {"key":"max_bitrate","type":"int","required":false","default":20000,"description":"Soft bitrate ceiling (kbps)","control":{"kind":"range","min":1000,"max":100000,"step":500,"unit":"kbps"}},
    {"key":"enabled","type":"bool","required":false,"default":true,"description":"Enable or disable relay","control":{"kind":"toggle"}}
  ]
}
