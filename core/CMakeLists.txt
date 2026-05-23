# ===========================================================================
# Core Library: arhud_core
# ===========================================================================
#
# arhud_core is the engine's lowest-level library with zero external deps, providing:
#   - Base type aliases and platform macros (typedefs.h)
#   - Global memory allocator (memory.h / memory.cpp)
#   - Atomic primitives (safe_refcount.h)
#   - Sync primitives (sync.h)
#   - Thread abstraction (thread.h / thread.cpp)
#   - Logger system (logger.h / logger.cpp)
#   - String utilities (ustring.h / ustring.cpp)
#   - Math functions (math_funcs.h)
#   - Template containers (PagedAllocator, LocalVector, HashMap, etc.)
#
# Dependency direction:
#   arhud_core (zero deps)
#       ^
#   arhud_servers (depends on core)     ← Rendering device interfaces
#       ^
#   arhud_drivers (depends on core+servers)  ← GPU driver implementations
#       ^
#   arhud_platform (depends on core+drivers)  ← Platform abstraction layer
#
# ===========================================================================

# --- Source file list ----------------------------------------------------
set(CORE_SOURCES
    os/memory.cpp                          # Global memory allocator implementation (Alloc/Free/Realloc)
    os/thread.cpp                          # Thread class implementation (Start/WaitToFinish/GetCallerId)
    io/logger.cpp                          # Logger system implementation (StdLogger, CompositeLogger, global API)
    io/logger_file.cpp                     # FileLogger implementation (async ring buffer + writer thread + rotation)
    string/ustring.cpp                     # String class implementation (UTF-32 string, UTF-8 conversion, operations)
)

# --- Header file list ----------------------------------------------------
set(CORE_HEADERS
    typedefs.h                             # Base type aliases (u8/u32/f32 etc.), platform macros, assert macros, Error enum
    os/memory.h                            # Memory allocator interface, MemoryStats, ARHUD_NEW/DELETE macros
    template/safe_refcount.h               # Atomic primitives (CpuPause, SafeNumeric<T>, SafeFlag)
    os/sync.h                              # Sync primitives (SpinLock + Guard, Mutex, MutexLock, Semaphore, RWLock)
    os/thread.h                            # Thread abstraction (Start/WaitToFinish, PlatformFunctions)
    io/logger.h                            # Logger system (LogLevel, Logger, StdLogger, CompositeLogger, ARHUD_LOG_* macros)
    io/logger_file.h                       # FileLogger (async ring buffer + writer thread + file rotation)
    template/paged_allocator.h             # PagedAllocator<T> (fixed-size object pool, O(1) alloc/free, optional SpinLock)
    template/local_vector.h                # LocalVector<T> (no-COW dynamic array, 1.5x growth, optional tight mode)
    template/hash_map.h                    # HashMap<K,V> (open addressing, linear probing, prime capacity)
    template/hash_set.h                    # HashSet<K> (open addressing, linear probing, prime capacity)
    template/bitfield.h                    # BitField<E> (type-safe bitwise flags wrapper for enum class)
    template/list.h                        # List<T> (doubly-linked list, Element-as-iterator, delayed Data allocation)
    template/fixed_vector.h                # FixedVector<T,N> (stack-allocated fixed-capacity vector, zero heap alloc)
    template/ring_buffer.h                 # RingBuffer<T> (power-of-2 FIFO ring buffer, bitmask wrapping)
    template/vector_view.h                 # VectorView<T> (non-owning read-only memory view, like std::span)
    template/rid.h                         # RID (opaque resource ID), RIDOwner<T>, DEFINE_ID macro
    string/ustring.h                       # String (UTF-32 string, LocalVector-based, UTF-8 conversion)
    math/math_funcs.h                      # Math library (Vec2/Vec3/Vec4/Mat4/Rect/Color/Transform2D)
    math/glm_types.h                       # GLM integration (type aliases, arhud↔glm conversion, SIMD math)
)

# --- Library target definition -------------------------------------------
add_library(arhud_core STATIC ${CORE_SOURCES} ${CORE_HEADERS})

# --- Compile definitions --------------------------------------------------
target_compile_definitions(arhud_core
    PUBLIC
        $<$<CONFIG:Debug>:ARHUD_DEBUG=1>
        $<$<CONFIG:Release,RelWithDebInfo,MinSizeRel>:ARHUD_DEBUG=0>
)

# --- Include directories ---------------------------------------------------
target_include_directories(arhud_core
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}         # Allow #include "typedefs.h" from any directory
        ${CMAKE_CURRENT_SOURCE_DIR}/os       # Allow #include "memory.h", "sync.h", etc.
        ${CMAKE_CURRENT_SOURCE_DIR}/io       # Allow #include "logger.h", etc.
        ${CMAKE_CURRENT_SOURCE_DIR}/string   # Allow #include "ustring.h"
        ${CMAKE_CURRENT_SOURCE_DIR}/template # Allow #include "paged_allocator.h", etc.
        ${CMAKE_CURRENT_SOURCE_DIR}/math     # Allow #include "math_funcs.h"
)

# --- External dependencies ------------------------------------------------
target_link_libraries(arhud_core PUBLIC glm::glm)
