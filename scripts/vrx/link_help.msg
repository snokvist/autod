{
  "cap":"link",
  "contract_version":"0.1",
  "config_path":"/etc/wfb.conf",
  "commands":[
    {"name":"start","description":"Start the WFB supervisor","args":[]},
    {"name":"stop","description":"Stop the WFB supervisor","args":[]},
    {"name":"restart","description":"Restart the WFB supervisor","args":[]},
    {"name":"enable","description":"Enable the WFB supervisor at boot","args":[]},
    {"name":"disable","description":"Disable the WFB supervisor at boot","args":[]},
    {"name":"status","description":"Report WFB supervisor service state","args":[]},
    {"name":"get","description":"Read a [parameters] setting from /etc/wfb.conf","args":[{"key":"name","type":"string","required":true}]},
    {"name":"set","description":"Update a [parameters] setting in /etc/wfb.conf","args":[{"key":"pair","type":"string","required":true,"description":"key=value or key value"}]},
    {"name":"help","description":"Describe WFB link controls","args":[]}
  ],
  "settings":[
    {"key":"rx_nics","type":"string","default":"wlx40a5ef2f229b","description":"Receive NIC names (CSV)","control":{"kind":"text","placeholder":"wlx..."}},
    {"key":"tx_nics","type":"string","default":"wlx40a5ef2f229b","description":"Transmit NIC names (CSV)","control":{"kind":"text","placeholder":"wlx..."}},
    {"key":"master_node","type":"string","default":"192.168.2.20","description":"Master node IP or hostname","control":{"kind":"text","placeholder":"192.168.2.20"}},
    {"key":"link_id","type":"int","default":7669206,"description":"Link identifier","control":{"kind":"text"}},
    {"key":"mcs","type":"int","default":2,"description":"Modulation coding scheme index","control":{"kind":"range","min":0,"max":15,"step":1}},
    {"key":"ldpc","type":"int","default":0,"description":"LDPC toggle (0/1)","control":{"kind":"toggle"}},
    {"key":"stbc","type":"int","default":0,"description":"STBC toggle (0/1)","control":{"kind":"toggle"}},
    {"key":"key_file","type":"string","default":"/etc/gs.key","description":"Path to the encryption key file","control":{"kind":"text","placeholder":"/etc/gs.key"}},
    {"key":"log_interval","type":"int","default":1000,"description":"Telemetry log interval (ms)","control":{"kind":"range","min":0,"max":5000,"step":50,"unit":"ms"}},
    {"key":"REGION","type":"string","default":"US","description":"Regulatory region","control":{"kind":"text","placeholder":"US"}},
    {"key":"CHANNEL","type":"int","default":161,"description":"Operating channel","control":{"kind":"range","min":1,"max":196,"step":1}},
    {"key":"BANDWIDTH","type":"string","default":"HT20","description":"PHY bandwidth profile","control":{"kind":"select","options":["HT20","HT40"],"multi":false}},
    {"key":"TXPOWER","type":"int","default":500,"description":"Transmit power (mW)","control":{"kind":"range","min":1,"max":2000,"step":1,"unit":"mW"}},
    {"key":"WFB_BW_MHZ","type":"int","default":20,"description":"WFB bandwidth (MHz)","control":{"kind":"range","min":5,"max":80,"step":5,"unit":"MHz"}}
  ],
  "notes":[
    "Service controls call systemctl against wfb_supervisor.service.",
    "Configuration commands only operate inside the [parameters] block of /etc/wfb.conf; other sections are preserved as-is."
  ]
}
