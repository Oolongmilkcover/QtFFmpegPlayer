# Additional clean files
cmake_minimum_required(VERSION 3.16)

if("${CONFIG}" STREQUAL "" OR "${CONFIG}" STREQUAL "Debug")
  file(REMOVE_RECURSE
  "CMakeFiles\\QtPlayer_autogen.dir\\AutogenUsed.txt"
  "CMakeFiles\\QtPlayer_autogen.dir\\ParseCache.txt"
  "QtPlayer_autogen"
  )
endif()
