{
  "cap":"pixelpilot_mini_rk",
  "contract_version":"0.1",
  "commands":[
    {"name":"start","description":"Start the pixelpilot_mini_rk service","args":[]},
    {"name":"stop","description":"Stop the pixelpilot_mini_rk service","args":[]},
    {"name":"restart","description":"Restart the pixelpilot_mini_rk service","args":[]},
    {"name":"toggle_osd","description":"Toggle the OSD overlay on or off (SIGUSR1)","args":[]},
    {"name":"toggle_recording","description":"Toggle recording on or off (SIGUSR2)","args":[]},
    {"name":"reboot","description":"Reboot the device immediately","args":[]},
    {"name":"shutdown","description":"Shut down the device immediately","args":[]},
    {"name":"help","description":"Describe pixelpilot_mini_rk commands","args":[]}
  ],
  "notes":[
    "Start/stop/restart wrap systemctl/service calls for pixelpilot_mini_rk.",
    "OSD toggles send SIGUSR1 to the pixelpilot_mini_rk process; recording toggles use SIGUSR2.",
    "Ensure the process is running before toggling or the commands will report an error.",
    "The reboot command issues 'reboot now' in the background.",
    "The shutdown command issues 'shutdown now' in the background."
  ]
}
