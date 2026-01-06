cmake_policy(SET CMP0063 NEW)

project(dbgen CXX C)

add_definitions(-DDBNAME=dss -DMAC -DORACLE -DTPCH)

add_library(
  bolt_dbgen STATIC
  dbgen.cpp
  dbgen_gunk.cpp
  bm_utils.cpp
  build.cpp
  permute.cpp
  rnd.cpp
  rng64.cpp
  speed_seed.cpp
  text.cpp)

target_include_directories(bolt_dbgen PRIVATE include)

# Suppress warnings when compiling dbgen.
target_compile_options(bolt_dbgen PRIVATE -w)
