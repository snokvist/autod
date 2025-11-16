{
  "cap":"sync",
  "contract_version":"0.1",
  "commands":[
    {"name":"help","description":"Describe sync orchestration commands","args":[]},
    {"name":"status","description":"Fetch the master's current /sync/slaves view","args":[]},
    {"name":"move","description":"Assign or move slave IDs into sync slots via POST /sync/push","args":[
      {"key":"slave_id","type":"string","description":"ID to move; repeat per assignment","required":true},
      {"key":"slot","type":"string","description":"Target slot number (1-10) or 'clear' to unassign","required":true}
    ]},
    {"name":"replay","description":"Force slots or specific slave IDs to rerun their slot command waterfall","args":[
      {"key":"slot","type":"int","description":"Slot to replay; repeat for multiple slots","required":false},
      {"key":"slave_id","type":"string","description":"Replay assignments for a given slave id","required":false}
    ]}
  ],
  "notes":[
    "Commands proxy the daemon's /sync endpoints via http://127.0.0.1:55667 by default.",
    "Override AUTOD_HTTP_BASE (or AUTOD_HTTP_HOST/AUTOD_HTTP_PORT) to point at a different listener before launching autod.",
    "Provide multiple slave_id/slot pairs to swap assignments atomically in a single request.",
    "GET /sync/slaves now includes per-slot metadata (labels plus prefer_id reservations) so you can spot reserved IDs even when placeholders occupy those slots."
  ]
}
