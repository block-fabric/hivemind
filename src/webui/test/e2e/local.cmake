# The page scripts of Hivemind's own pages, run against a live mainchain as webui_e2e_<name>
# (webui_local_e2e of test/CMakeLists.txt, from the template).
# hivemind.js: markets made, traded, voted on and paid out, with periods of 45 seconds.
webui_local_e2e(hivemind 29740 --app-arg=-hivemindperiodseconds=45 --js-var PERIOD=45)
# hivemind_simple.js: Simple mode (the default): questions opened through RPC, a bet with Yes, My bets, a
# cash-out, in Midnight and Light; periods long enough that the questions stay open.
webui_local_e2e(hivemind_simple 41720 --app-arg=-hivemindperiodseconds=600 --js-var SIMPLE=true)
