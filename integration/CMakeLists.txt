cmake_minimum_required(VERSION 3.16)

# Custom 16 MB partition table
set(PARTITION_TABLE_CSV_FILEPATH "${CMAKE_CURRENT_LIST_DIR}/partitions.csv")
set(CONFIG_PARTITION_TABLE_CUSTOM y)
set(CONFIG_ESPTOOLPY_FLASHSIZE_16MB y)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(integration)
