{
  "cap":"pixelpilot_mini_rk",
  "contract_version":"0.1",
  "commands":[
    {"name":"start","description":"Start the pixelpilot_mini_rk service","args":[]},
    {"name":"stop","description":"Stop the pixelpilot_mini_rk service","args":[]},
    {"name":"restart","description":"Restart the pixelpilot_mini_rk service","args":[]},
    {"name":"toggle_osd","description":"Toggle the OSD overlay on or off (SIGUSR1)","args":[]},
    {"name":"toggle_recording","description":"Toggle recording on or off (SIGUSR2)","args":[]},
    {"name":"gamma","description":"Run /usr/bin/gamma with an optional preset (pass --list to enumerate)","args":["preset|--list"]},
    {"name":"help","description":"Describe pixelpilot_mini_rk commands","args":[]}
  ],
  "notes":[
    "Start/stop/restart wrap systemctl/service calls for pixelpilot_mini_rk.",
    "OSD toggles send SIGUSR1 to the pixelpilot_mini_rk process; recording toggles use SIGUSR2.",
    "Use 'gamma --list' to fetch available presets. Pass a preset name to apply it immediately.",
    "Ensure the process is running before toggling or the commands will report an error.",
    "Device-level reboot and shutdown controls are available via /sys/help."
  ]
}
