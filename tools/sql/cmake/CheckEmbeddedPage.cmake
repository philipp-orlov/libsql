# Fails when the page embedded into the sql binary (public/index.html) still carries the retired protocol client or
# lacks the librpc one: it is built from ui/ by `npm run b`, and an old copy would be refused by the server.
file(READ "${PAGE}" page)
string(FIND "${page}" "librpc.v1" new)
string(FIND "${page}" "libsql.v1" old)
if(new EQUAL -1)
    message(FATAL_ERROR "${PAGE} does not speak librpc.v1: run `npm run b` in tools/sql/ui")
endif()
if(NOT old EQUAL -1)
    message(FATAL_ERROR "${PAGE} still contains the retired libsql.v1 client: run `npm run b` in tools/sql/ui")
endif()
