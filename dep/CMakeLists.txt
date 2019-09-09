add_subdirectory(glad)
add_subdirectory(googletest)
add_subdirectory(imgui)
add_subdirectory(inih)
add_subdirectory(libsamplerate)
add_subdirectory(nativefiledialog)
add_subdirectory(softfloat)
add_subdirectory(xxhash)

###################### YBaseLib ############################

set(YBASELIB_SRC_BASE ${CMAKE_SOURCE_DIR}/dep/YBaseLib/Source)
set(YBASELIB_INCLUDES ${CMAKE_SOURCE_DIR}/dep/YBaseLib/Include)
set(YBASELIB_SRC_FILES
    ${YBASELIB_SRC_BASE}/YBaseLib/Android
    ${YBASELIB_SRC_BASE}/YBaseLib/Android/AndroidConditionVariable.cpp
    ${YBASELIB_SRC_BASE}/YBaseLib/Android/AndroidEvent.cpp
    ${YBASELIB_SRC_BASE}/YBaseLib/Android/AndroidFileSystem.cpp
    ${YBASELIB_SRC_BASE}/YBaseLib/Android/AndroidPlatform.cpp
    ${YBASELIB_SRC_BASE}/YBaseLib/Android/AndroidReadWriteLock.cpp
    ${YBASELIB_SRC_BASE}/YBaseLib/Android/AndroidThread.cpp
    ${YBASELIB_SRC_BASE}/YBaseLib/Assert.cpp
    ${YBASELIB_SRC_BASE}/YBaseLib/Atomic.cpp
    ${YBASELIB_SRC_BASE}/YBaseLib/BinaryBlob.cpp
    ${YBASELIB_SRC_BASE}/YBaseLib/BinaryReadBuffer.cpp
    ${YBASELIB_SRC_BASE}/YBaseLib/BinaryReader.cpp
    ${YBASELIB_SRC_BASE}/YBaseLib/BinaryWriteBuffer.cpp
    ${YBASELIB_SRC_BASE}/YBaseLib/BinaryWriter.cpp
    ${YBASELIB_SRC_BASE}/YBaseLib/ByteStream.cpp
    ${YBASELIB_SRC_BASE}/YBaseLib/CallbackQueue.cpp
    ${YBASELIB_SRC_BASE}/YBaseLib/CircularBuffer.cpp
    ${YBASELIB_SRC_BASE}/YBaseLib/CPUID.cpp
    ${YBASELIB_SRC_BASE}/YBaseLib/CRC32.cpp
    ${YBASELIB_SRC_BASE}/YBaseLib/CString.cpp
    ${YBASELIB_SRC_BASE}/YBaseLib/Endian.cpp
    ${YBASELIB_SRC_BASE}/YBaseLib/Error.cpp
    ${YBASELIB_SRC_BASE}/YBaseLib/Exception.cpp
    ${YBASELIB_SRC_BASE}/YBaseLib/FileSystem.cpp
    ${YBASELIB_SRC_BASE}/YBaseLib/HashTrait.cpp
    ${YBASELIB_SRC_BASE}/YBaseLib/HTML5
    ${YBASELIB_SRC_BASE}/YBaseLib/HTML5/HTML5Barrier.cpp
    ${YBASELIB_SRC_BASE}/YBaseLib/HTML5/HTML5ConditionVariable.cpp
    ${YBASELIB_SRC_BASE}/YBaseLib/HTML5/HTML5FileSystem.cpp
    ${YBASELIB_SRC_BASE}/YBaseLib/HTML5/HTML5Platform.cpp
    ${YBASELIB_SRC_BASE}/YBaseLib/HTML5/HTML5ReadWriteLock.cpp
    ${YBASELIB_SRC_BASE}/YBaseLib/HTML5/HTML5Thread.cpp
    ${YBASELIB_SRC_BASE}/YBaseLib/Log.cpp
    ${YBASELIB_SRC_BASE}/YBaseLib/Math.cpp
    ${YBASELIB_SRC_BASE}/YBaseLib/MD5Digest.cpp
    ${YBASELIB_SRC_BASE}/YBaseLib/Memory.cpp
    ${YBASELIB_SRC_BASE}/YBaseLib/NameTable.cpp
    ${YBASELIB_SRC_BASE}/YBaseLib/NumericLimits.cpp
    ${YBASELIB_SRC_BASE}/YBaseLib/POSIX
    ${YBASELIB_SRC_BASE}/YBaseLib/POSIX/POSIXConditionVariable.cpp
    ${YBASELIB_SRC_BASE}/YBaseLib/POSIX/POSIXFileSystem.cpp
    ${YBASELIB_SRC_BASE}/YBaseLib/POSIX/POSIXPlatform.cpp
    ${YBASELIB_SRC_BASE}/YBaseLib/POSIX/POSIXReadWriteLock.cpp
    ${YBASELIB_SRC_BASE}/YBaseLib/POSIX/POSIXSubprocess.cpp
    ${YBASELIB_SRC_BASE}/YBaseLib/POSIX/POSIXThread.cpp
    ${YBASELIB_SRC_BASE}/YBaseLib/ProgressCallbacks.cpp
    ${YBASELIB_SRC_BASE}/YBaseLib/ReferenceCounted.cpp
    ${YBASELIB_SRC_BASE}/YBaseLib/Sockets
    ${YBASELIB_SRC_BASE}/YBaseLib/Sockets/Generic
    ${YBASELIB_SRC_BASE}/YBaseLib/Sockets/Generic/BufferedStreamSocket.cpp
    ${YBASELIB_SRC_BASE}/YBaseLib/Sockets/Generic/ListenSocket.cpp
    ${YBASELIB_SRC_BASE}/YBaseLib/Sockets/Generic/SocketMultiplexer.cpp
    ${YBASELIB_SRC_BASE}/YBaseLib/Sockets/Generic/StreamSocket.cpp
    ${YBASELIB_SRC_BASE}/YBaseLib/Sockets/SocketAddress.cpp
    ${YBASELIB_SRC_BASE}/YBaseLib/StringConverter.cpp
    ${YBASELIB_SRC_BASE}/YBaseLib/String.cpp
    ${YBASELIB_SRC_BASE}/YBaseLib/StringParser.cpp
    ${YBASELIB_SRC_BASE}/YBaseLib/TaskQueue.cpp
    ${YBASELIB_SRC_BASE}/YBaseLib/TextReader.cpp
    ${YBASELIB_SRC_BASE}/YBaseLib/TextWriter.cpp
    ${YBASELIB_SRC_BASE}/YBaseLib/ThreadPool.cpp
    ${YBASELIB_SRC_BASE}/YBaseLib/Timer.cpp
    ${YBASELIB_SRC_BASE}/YBaseLib/Timestamp.cpp
    ${YBASELIB_SRC_BASE}/YBaseLib/Windows
    ${YBASELIB_SRC_BASE}/YBaseLib/Windows/WindowsBarrier.cpp
    ${YBASELIB_SRC_BASE}/YBaseLib/Windows/WindowsConditionVariable.cpp
    ${YBASELIB_SRC_BASE}/YBaseLib/Windows/WindowsFileSystem.cpp
    ${YBASELIB_SRC_BASE}/YBaseLib/Windows/WindowsPlatform.cpp
    ${YBASELIB_SRC_BASE}/YBaseLib/Windows/WindowsReadWriteLock.cpp
    ${YBASELIB_SRC_BASE}/YBaseLib/Windows/WindowsSubprocess.cpp
    ${YBASELIB_SRC_BASE}/YBaseLib/Windows/WindowsThread.cpp
)

add_library(YBaseLib STATIC ${YBASELIB_SRC_FILES})
target_include_directories(YBaseLib PRIVATE ${YBASELIB_INCLUDES} ${YBASELIB_SRC_BASE})
target_include_directories(YBaseLib PUBLIC ${YBASELIB_INCLUDES})
target_link_libraries(YBaseLib Threads::Threads)
