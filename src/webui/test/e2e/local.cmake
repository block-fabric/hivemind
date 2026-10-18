# The page scripts of Hivemind's own pages, run against a live mainchain as webui_e2e_<name>
# (webui_local_e2e of test/CMakeLists.txt, from the template).
# hivemind.js: markets made, traded, voted on and paid out, with periods of 45 seconds.
webui_local_e2e(hivemind 29740 --app-arg=-hivemindperiodseconds=45 --js-var PERIOD=45)
# hivemind_simple.js: Simple mode (the default): questions opened through RPC, a bet with Yes, My bets, a
# cash-out, in Midnight and Light; periods long enough that the questions stay open.
webui_local_e2e(hivemind_simple 41720 --app-arg=-hivemindperiodseconds=600 --js-var SIMPLE=true)
# quit_prepare.js then run_quit.py: a market still being created asks before a quit, in the dialog of the host.
add_test(NAME webui_e2e_quit_hivemind
  COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/e2e/run_quit.py
    --app $<TARGET_FILE:chains-app>
    --tmp ${CMAKE_CURRENT_BINARY_DIR}/e2e-quit-hivemind
    --shots ${CMAKE_CURRENT_BINARY_DIR}/e2e-shots
    --port 49630
    --prepare ${CMAKE_CURRENT_SOURCE_DIR}/e2e/quit_prepare.js
    --expect "A market is still being created ([Weather] Snow in Paris). If you quit now it resumes the next time you open Hivemind."
)
set_tests_properties(webui_e2e_quit_hivemind PROPERTIES TIMEOUT 1500 RUN_SERIAL TRUE)
