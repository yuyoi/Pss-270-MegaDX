// Shared look for every page, taken from the PSS-270's own panel: black body, cream keys,
// green voice buttons, gold rhythm buttons, lavender tempo buttons, cyan display print.
// Served once at /s.css so the pages stay small and always match.
#pragma once
const char THEME_CSS[] PROGMEM = R"CSS(
:root{--body:#121211;--panel:#1c1b18;--line:#3a382f;--text:#ebe5d3;--dim:#938d7a;
--green:#2f9a68;--green-d:#1d6444;--gold:#a8944a;--gold-d:#6f5f2b;--lav:#8b8fb3;--lav-d:#565a7e;--cyan:#58d3c8}
*{box-sizing:border-box}
body{font-family:system-ui,sans-serif;margin:14px;background:var(--body);color:var(--text)}
h2{margin:6px 0 12px;font-size:19px;font-weight:700;letter-spacing:.06em;text-transform:uppercase}
a{color:var(--cyan);font-size:14px;margin-left:14px;text-decoration:none;font-weight:400;letter-spacing:0;text-transform:none}
a:hover{text-decoration:underline}
button{background:var(--gold);color:#17140a;border:0;border-radius:5px;padding:9px 14px;margin:3px;font-size:15px;font-weight:600;box-shadow:0 2px 0 var(--gold-d);cursor:pointer}
button:active{transform:translateY(2px);box-shadow:none}
button.go,button.on{background:var(--green);color:#fff;box-shadow:0 2px 0 var(--green-d)}
button.alt{background:var(--lav);color:#12132a;box-shadow:0 2px 0 var(--lav-d)}
input,select{background:#0d0d0c;color:var(--text);border:1px solid var(--line);border-radius:6px;padding:8px 10px;font-size:15px}
input:focus,select:focus{outline:1px solid var(--cyan)}
input[type=range],input[type=checkbox]{accent-color:var(--green)}
.row{margin:9px 0}
.card,.col{background:var(--panel);border:1px solid var(--line);border-radius:8px;padding:10px 14px}
.card{margin:12px 0}
.card h3,.col h3{margin:2px 0 8px;font-size:14px;font-weight:700;letter-spacing:.07em;text-transform:uppercase;color:var(--cyan)}
.dim{color:var(--dim);font-size:13px}
.bad{color:#ff8a7a}
.ok{color:var(--green)}
.v{color:var(--cyan)}
)CSS";
