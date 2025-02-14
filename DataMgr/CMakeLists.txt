if(NOT MSVC)
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -Wno-unused-result")
endif()

set(datamgr_source_files
    AbstractBuffer.cpp
    Allocators/CudaAllocator.cpp
    Allocators/ThrustAllocator.cpp
    Chunk/Chunk.cpp
    DataMgr.cpp
    Encoder.cpp
    StringNoneEncoder.cpp
    FileMgr/CachingFileMgr.cpp
    FileMgr/GlobalFileMgr.cpp
    FileMgr/CachingGlobalFileMgr.cpp
    FileMgr/FileMgr.cpp
    FileMgr/FileBuffer.cpp
    FileMgr/FileInfo.cpp
    ForeignStorage/AbstractTextFileDataWrapper.cpp
    ForeignStorage/ArrowForeignStorage.cpp
    ForeignStorage/CacheEvictionAlgorithms/LRUEvictionAlgorithm.cpp
    ForeignStorage/CsvDataWrapper.cpp
    ForeignStorage/CachingForeignStorageMgr.cpp
    ForeignStorage/DummyForeignStorage.cpp
    ForeignStorage/ForeignStorageInterface.cpp
    ForeignStorage/ForeignStorageBuffer.cpp
    ForeignStorage/ForeignStorageMgr.cpp
    ForeignStorage/ForeignStorageCache.cpp
    ForeignStorage/FsiChunkUtils.cpp
    ForeignStorage/FsiJsonUtils.cpp
    ForeignStorage/FileReader.cpp
    ForeignStorage/FileRegions.cpp
    ForeignStorage/TextFileBufferParser.cpp
    ForeignStorage/CsvFileBufferParser.cpp
    BufferMgr/GpuCudaBufferMgr/GpuCudaBufferMgr.cpp
    BufferMgr/GpuCudaBufferMgr/GpuCudaBuffer.cpp
    BufferMgr/CpuBufferMgr/CpuBufferMgr.cpp
    BufferMgr/CpuBufferMgr/CpuBuffer.cpp
    BufferMgr/CpuBufferMgr/TieredCpuBufferMgr.cpp
    BufferMgr/BufferMgr.cpp
    BufferMgr/Buffer.cpp
    PersistentStorageMgr/PersistentStorageMgr.cpp
    ForeignStorage/ForeignTableRefresh.cpp
    ForeignStorage/AbstractFileStorageDataWrapper.cpp
    ForeignStorage/ForeignDataWrapperFactory.cpp
    ForeignStorage/InternalCatalogDataWrapper.cpp
    ForeignStorage/RegexParserDataWrapper.cpp
    ForeignStorage/RegexFileBufferParser.cpp
)

if(ENABLE_AWS_S3)
  list(APPEND datamgr_source_files ForeignStorage/S3FilePathUtil.cpp)
endif()

if (ENABLE_MEMKIND)
  list(APPEND datamgr_source_files Allocators/PMemAllocator.cpp)
endif()

if("${MAPD_EDITION_LOWER}" STREQUAL "ee")
if(ENABLE_AWS_S3)
  list(APPEND datamgr_source_files ForeignStorage/ee/FileReaderS3.cpp)
  list(APPEND datamgr_source_files ForeignStorage/ee/S3Utils.cpp)
  list(APPEND datamgr_source_files ForeignStorage/ee/S3SelectClient.cpp)
  list(APPEND datamgr_source_files ForeignStorage/ee/S3SelectDataWrapper.cpp)
endif()
endif()

if(ENABLE_IMPORT_PARQUET)
  list(APPEND datamgr_source_files ForeignStorage/ParquetDataWrapper.cpp)
  list(APPEND datamgr_source_files ForeignStorage/ParquetShared.cpp)
  list(APPEND datamgr_source_files ForeignStorage/LazyParquetChunkLoader.cpp)
  list(APPEND datamgr_source_files ForeignStorage/TypedParquetStorageBuffer.cpp)
  list(APPEND datamgr_source_files ForeignStorage/ParquetImporter.cpp)
endif()

# On Linux, Arrow internally links symbols for its dependency libraries (such as the C++ AWS SDK)
# when it is built as a shared library. In this case, set a flag that would be used to determine
# how to handle AWS SDK initialization and shutdown.
function(set_arrow_has_private_aws_sdk)
  if(${CMAKE_SYSTEM_NAME} MATCHES "Linux")
    foreach(LIB_PATH ${Arrow_LIBRARIES})
      get_filename_component(LIB_NAME ${LIB_PATH} NAME)
      if (${LIB_NAME} STREQUAL "${CMAKE_SHARED_LIBRARY_PREFIX}arrow${CMAKE_SHARED_LIBRARY_SUFFIX}")
        add_definitions("-DARROW_HAS_PRIVATE_AWS_SDK")
      endif()
    endforeach()
  endif()
endfunction()

if(ENABLE_AWS_S3)
  list(APPEND datamgr_source_files OmniSciAwsSdk.cpp)
  set_arrow_has_private_aws_sdk()
endif()

add_library(DataMgr ${datamgr_source_files})

target_link_libraries(DataMgr CudaMgr "$<$<BOOL:${ENABLE_FOLLY}>:${Folly_LIBRARIES}>" Shared ${Boost_THREAD_LIBRARY} ${TBB_LIBS} ${CMAKE_DL_LIBS} "$<$<BOOL:${ENABLE_MEMKIND}>:${memkind_LIBRARIES}>")

option(ENABLE_CRASH_CORRUPTION_TEST "Enable crash using SIGUSR2 during page deletion to faster and affirmative test/repro db corruption" OFF)
if(ENABLE_CRASH_CORRUPTION_TEST)
  add_definitions("-DENABLE_CRASH_CORRUPTION_TEST")
endif()
