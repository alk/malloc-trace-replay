// Generated by Cap'n Proto compiler, DO NOT EDIT
// source: replay.capnp

#ifndef CAPNP_INCLUDED_cb49ade5c790d703_
#define CAPNP_INCLUDED_cb49ade5c790d703_

#include <capnp/generated-header-support.h>

#if CAPNP_VERSION != 6001
#error "Version mismatch between generated code and library headers.  You must use the same version of the Cap'n Proto compiler and library."
#endif


namespace capnp {
namespace schemas {

CAPNP_DECLARE_SCHEMA(e1db17f6470d97a8);
CAPNP_DECLARE_SCHEMA(9cdbae3f4ce29414);
enum class Type_9cdbae3f4ce29414: uint16_t {
  MALLOC,
  FREE,
  REALLOC,
  MEMALLIGN,
};
CAPNP_DECLARE_ENUM(Type, 9cdbae3f4ce29414);
CAPNP_DECLARE_SCHEMA(d7eac18caea825d2);
CAPNP_DECLARE_SCHEMA(f599a53b0bdaadae);

}  // namespace schemas
}  // namespace capnp

namespace replay {

struct Instruction {
  Instruction() = delete;

  class Reader;
  class Builder;
  class Pipeline;
  typedef ::capnp::schemas::Type_9cdbae3f4ce29414 Type;


  struct _capnpPrivate {
    CAPNP_DECLARE_STRUCT_HEADER(e1db17f6470d97a8, 4, 0)
    #if !CAPNP_LITE
    static constexpr ::capnp::_::RawBrandedSchema const* brand() { return &schema->defaultBrand; }
    #endif  // !CAPNP_LITE
  };
};

struct ThreadChunk {
  ThreadChunk() = delete;

  class Reader;
  class Builder;
  class Pipeline;

  struct _capnpPrivate {
    CAPNP_DECLARE_STRUCT_HEADER(d7eac18caea825d2, 2, 1)
    #if !CAPNP_LITE
    static constexpr ::capnp::_::RawBrandedSchema const* brand() { return &schema->defaultBrand; }
    #endif  // !CAPNP_LITE
  };
};

struct Batch {
  Batch() = delete;

  class Reader;
  class Builder;
  class Pipeline;

  struct _capnpPrivate {
    CAPNP_DECLARE_STRUCT_HEADER(f599a53b0bdaadae, 0, 1)
    #if !CAPNP_LITE
    static constexpr ::capnp::_::RawBrandedSchema const* brand() { return &schema->defaultBrand; }
    #endif  // !CAPNP_LITE
  };
};

// =======================================================================================

class Instruction::Reader {
public:
  typedef Instruction Reads;

  Reader() = default;
  inline explicit Reader(::capnp::_::StructReader base): _reader(base) {}

  inline ::capnp::MessageSize totalSize() const {
    return _reader.totalSize().asPublic();
  }

#if !CAPNP_LITE
  inline ::kj::StringTree toString() const {
    return ::capnp::_::structString(_reader, *_capnpPrivate::brand());
  }
#endif  // !CAPNP_LITE

  inline  ::replay::Instruction::Type getType() const;

  inline  ::uint32_t getReg() const;

  inline  ::uint64_t getSize() const;

  inline  ::uint64_t getAlignment() const;

  inline  ::uint32_t getOldReg() const;

private:
  ::capnp::_::StructReader _reader;
  template <typename, ::capnp::Kind>
  friend struct ::capnp::ToDynamic_;
  template <typename, ::capnp::Kind>
  friend struct ::capnp::_::PointerHelpers;
  template <typename, ::capnp::Kind>
  friend struct ::capnp::List;
  friend class ::capnp::MessageBuilder;
  friend class ::capnp::Orphanage;
};

class Instruction::Builder {
public:
  typedef Instruction Builds;

  Builder() = delete;  // Deleted to discourage incorrect usage.
                       // You can explicitly initialize to nullptr instead.
  inline Builder(decltype(nullptr)) {}
  inline explicit Builder(::capnp::_::StructBuilder base): _builder(base) {}
  inline operator Reader() const { return Reader(_builder.asReader()); }
  inline Reader asReader() const { return *this; }

  inline ::capnp::MessageSize totalSize() const { return asReader().totalSize(); }
#if !CAPNP_LITE
  inline ::kj::StringTree toString() const { return asReader().toString(); }
#endif  // !CAPNP_LITE

  inline  ::replay::Instruction::Type getType();
  inline void setType( ::replay::Instruction::Type value);

  inline  ::uint32_t getReg();
  inline void setReg( ::uint32_t value);

  inline  ::uint64_t getSize();
  inline void setSize( ::uint64_t value);

  inline  ::uint64_t getAlignment();
  inline void setAlignment( ::uint64_t value);

  inline  ::uint32_t getOldReg();
  inline void setOldReg( ::uint32_t value);

private:
  ::capnp::_::StructBuilder _builder;
  template <typename, ::capnp::Kind>
  friend struct ::capnp::ToDynamic_;
  friend class ::capnp::Orphanage;
  template <typename, ::capnp::Kind>
  friend struct ::capnp::_::PointerHelpers;
};

#if !CAPNP_LITE
class Instruction::Pipeline {
public:
  typedef Instruction Pipelines;

  inline Pipeline(decltype(nullptr)): _typeless(nullptr) {}
  inline explicit Pipeline(::capnp::AnyPointer::Pipeline&& typeless)
      : _typeless(kj::mv(typeless)) {}

private:
  ::capnp::AnyPointer::Pipeline _typeless;
  friend class ::capnp::PipelineHook;
  template <typename, ::capnp::Kind>
  friend struct ::capnp::ToDynamic_;
};
#endif  // !CAPNP_LITE

class ThreadChunk::Reader {
public:
  typedef ThreadChunk Reads;

  Reader() = default;
  inline explicit Reader(::capnp::_::StructReader base): _reader(base) {}

  inline ::capnp::MessageSize totalSize() const {
    return _reader.totalSize().asPublic();
  }

#if !CAPNP_LITE
  inline ::kj::StringTree toString() const {
    return ::capnp::_::structString(_reader, *_capnpPrivate::brand());
  }
#endif  // !CAPNP_LITE

  inline  ::uint64_t getThreadID() const;

  inline bool getLive() const;

  inline bool hasInstructions() const;
  inline  ::capnp::List< ::replay::Instruction>::Reader getInstructions() const;

private:
  ::capnp::_::StructReader _reader;
  template <typename, ::capnp::Kind>
  friend struct ::capnp::ToDynamic_;
  template <typename, ::capnp::Kind>
  friend struct ::capnp::_::PointerHelpers;
  template <typename, ::capnp::Kind>
  friend struct ::capnp::List;
  friend class ::capnp::MessageBuilder;
  friend class ::capnp::Orphanage;
};

class ThreadChunk::Builder {
public:
  typedef ThreadChunk Builds;

  Builder() = delete;  // Deleted to discourage incorrect usage.
                       // You can explicitly initialize to nullptr instead.
  inline Builder(decltype(nullptr)) {}
  inline explicit Builder(::capnp::_::StructBuilder base): _builder(base) {}
  inline operator Reader() const { return Reader(_builder.asReader()); }
  inline Reader asReader() const { return *this; }

  inline ::capnp::MessageSize totalSize() const { return asReader().totalSize(); }
#if !CAPNP_LITE
  inline ::kj::StringTree toString() const { return asReader().toString(); }
#endif  // !CAPNP_LITE

  inline  ::uint64_t getThreadID();
  inline void setThreadID( ::uint64_t value);

  inline bool getLive();
  inline void setLive(bool value);

  inline bool hasInstructions();
  inline  ::capnp::List< ::replay::Instruction>::Builder getInstructions();
  inline void setInstructions( ::capnp::List< ::replay::Instruction>::Reader value);
  inline  ::capnp::List< ::replay::Instruction>::Builder initInstructions(unsigned int size);
  inline void adoptInstructions(::capnp::Orphan< ::capnp::List< ::replay::Instruction>>&& value);
  inline ::capnp::Orphan< ::capnp::List< ::replay::Instruction>> disownInstructions();

private:
  ::capnp::_::StructBuilder _builder;
  template <typename, ::capnp::Kind>
  friend struct ::capnp::ToDynamic_;
  friend class ::capnp::Orphanage;
  template <typename, ::capnp::Kind>
  friend struct ::capnp::_::PointerHelpers;
};

#if !CAPNP_LITE
class ThreadChunk::Pipeline {
public:
  typedef ThreadChunk Pipelines;

  inline Pipeline(decltype(nullptr)): _typeless(nullptr) {}
  inline explicit Pipeline(::capnp::AnyPointer::Pipeline&& typeless)
      : _typeless(kj::mv(typeless)) {}

private:
  ::capnp::AnyPointer::Pipeline _typeless;
  friend class ::capnp::PipelineHook;
  template <typename, ::capnp::Kind>
  friend struct ::capnp::ToDynamic_;
};
#endif  // !CAPNP_LITE

class Batch::Reader {
public:
  typedef Batch Reads;

  Reader() = default;
  inline explicit Reader(::capnp::_::StructReader base): _reader(base) {}

  inline ::capnp::MessageSize totalSize() const {
    return _reader.totalSize().asPublic();
  }

#if !CAPNP_LITE
  inline ::kj::StringTree toString() const {
    return ::capnp::_::structString(_reader, *_capnpPrivate::brand());
  }
#endif  // !CAPNP_LITE

  inline bool hasThreads() const;
  inline  ::capnp::List< ::replay::ThreadChunk>::Reader getThreads() const;

private:
  ::capnp::_::StructReader _reader;
  template <typename, ::capnp::Kind>
  friend struct ::capnp::ToDynamic_;
  template <typename, ::capnp::Kind>
  friend struct ::capnp::_::PointerHelpers;
  template <typename, ::capnp::Kind>
  friend struct ::capnp::List;
  friend class ::capnp::MessageBuilder;
  friend class ::capnp::Orphanage;
};

class Batch::Builder {
public:
  typedef Batch Builds;

  Builder() = delete;  // Deleted to discourage incorrect usage.
                       // You can explicitly initialize to nullptr instead.
  inline Builder(decltype(nullptr)) {}
  inline explicit Builder(::capnp::_::StructBuilder base): _builder(base) {}
  inline operator Reader() const { return Reader(_builder.asReader()); }
  inline Reader asReader() const { return *this; }

  inline ::capnp::MessageSize totalSize() const { return asReader().totalSize(); }
#if !CAPNP_LITE
  inline ::kj::StringTree toString() const { return asReader().toString(); }
#endif  // !CAPNP_LITE

  inline bool hasThreads();
  inline  ::capnp::List< ::replay::ThreadChunk>::Builder getThreads();
  inline void setThreads( ::capnp::List< ::replay::ThreadChunk>::Reader value);
  inline  ::capnp::List< ::replay::ThreadChunk>::Builder initThreads(unsigned int size);
  inline void adoptThreads(::capnp::Orphan< ::capnp::List< ::replay::ThreadChunk>>&& value);
  inline ::capnp::Orphan< ::capnp::List< ::replay::ThreadChunk>> disownThreads();

private:
  ::capnp::_::StructBuilder _builder;
  template <typename, ::capnp::Kind>
  friend struct ::capnp::ToDynamic_;
  friend class ::capnp::Orphanage;
  template <typename, ::capnp::Kind>
  friend struct ::capnp::_::PointerHelpers;
};

#if !CAPNP_LITE
class Batch::Pipeline {
public:
  typedef Batch Pipelines;

  inline Pipeline(decltype(nullptr)): _typeless(nullptr) {}
  inline explicit Pipeline(::capnp::AnyPointer::Pipeline&& typeless)
      : _typeless(kj::mv(typeless)) {}

private:
  ::capnp::AnyPointer::Pipeline _typeless;
  friend class ::capnp::PipelineHook;
  template <typename, ::capnp::Kind>
  friend struct ::capnp::ToDynamic_;
};
#endif  // !CAPNP_LITE

// =======================================================================================

inline  ::replay::Instruction::Type Instruction::Reader::getType() const {
  return _reader.getDataField< ::replay::Instruction::Type>(
      ::capnp::bounded<0>() * ::capnp::ELEMENTS);
}

inline  ::replay::Instruction::Type Instruction::Builder::getType() {
  return _builder.getDataField< ::replay::Instruction::Type>(
      ::capnp::bounded<0>() * ::capnp::ELEMENTS);
}
inline void Instruction::Builder::setType( ::replay::Instruction::Type value) {
  _builder.setDataField< ::replay::Instruction::Type>(
      ::capnp::bounded<0>() * ::capnp::ELEMENTS, value);
}

inline  ::uint32_t Instruction::Reader::getReg() const {
  return _reader.getDataField< ::uint32_t>(
      ::capnp::bounded<1>() * ::capnp::ELEMENTS);
}

inline  ::uint32_t Instruction::Builder::getReg() {
  return _builder.getDataField< ::uint32_t>(
      ::capnp::bounded<1>() * ::capnp::ELEMENTS);
}
inline void Instruction::Builder::setReg( ::uint32_t value) {
  _builder.setDataField< ::uint32_t>(
      ::capnp::bounded<1>() * ::capnp::ELEMENTS, value);
}

inline  ::uint64_t Instruction::Reader::getSize() const {
  return _reader.getDataField< ::uint64_t>(
      ::capnp::bounded<1>() * ::capnp::ELEMENTS);
}

inline  ::uint64_t Instruction::Builder::getSize() {
  return _builder.getDataField< ::uint64_t>(
      ::capnp::bounded<1>() * ::capnp::ELEMENTS);
}
inline void Instruction::Builder::setSize( ::uint64_t value) {
  _builder.setDataField< ::uint64_t>(
      ::capnp::bounded<1>() * ::capnp::ELEMENTS, value);
}

inline  ::uint64_t Instruction::Reader::getAlignment() const {
  return _reader.getDataField< ::uint64_t>(
      ::capnp::bounded<2>() * ::capnp::ELEMENTS);
}

inline  ::uint64_t Instruction::Builder::getAlignment() {
  return _builder.getDataField< ::uint64_t>(
      ::capnp::bounded<2>() * ::capnp::ELEMENTS);
}
inline void Instruction::Builder::setAlignment( ::uint64_t value) {
  _builder.setDataField< ::uint64_t>(
      ::capnp::bounded<2>() * ::capnp::ELEMENTS, value);
}

inline  ::uint32_t Instruction::Reader::getOldReg() const {
  return _reader.getDataField< ::uint32_t>(
      ::capnp::bounded<6>() * ::capnp::ELEMENTS);
}

inline  ::uint32_t Instruction::Builder::getOldReg() {
  return _builder.getDataField< ::uint32_t>(
      ::capnp::bounded<6>() * ::capnp::ELEMENTS);
}
inline void Instruction::Builder::setOldReg( ::uint32_t value) {
  _builder.setDataField< ::uint32_t>(
      ::capnp::bounded<6>() * ::capnp::ELEMENTS, value);
}

inline  ::uint64_t ThreadChunk::Reader::getThreadID() const {
  return _reader.getDataField< ::uint64_t>(
      ::capnp::bounded<0>() * ::capnp::ELEMENTS);
}

inline  ::uint64_t ThreadChunk::Builder::getThreadID() {
  return _builder.getDataField< ::uint64_t>(
      ::capnp::bounded<0>() * ::capnp::ELEMENTS);
}
inline void ThreadChunk::Builder::setThreadID( ::uint64_t value) {
  _builder.setDataField< ::uint64_t>(
      ::capnp::bounded<0>() * ::capnp::ELEMENTS, value);
}

inline bool ThreadChunk::Reader::getLive() const {
  return _reader.getDataField<bool>(
      ::capnp::bounded<64>() * ::capnp::ELEMENTS);
}

inline bool ThreadChunk::Builder::getLive() {
  return _builder.getDataField<bool>(
      ::capnp::bounded<64>() * ::capnp::ELEMENTS);
}
inline void ThreadChunk::Builder::setLive(bool value) {
  _builder.setDataField<bool>(
      ::capnp::bounded<64>() * ::capnp::ELEMENTS, value);
}

inline bool ThreadChunk::Reader::hasInstructions() const {
  return !_reader.getPointerField(
      ::capnp::bounded<0>() * ::capnp::POINTERS).isNull();
}
inline bool ThreadChunk::Builder::hasInstructions() {
  return !_builder.getPointerField(
      ::capnp::bounded<0>() * ::capnp::POINTERS).isNull();
}
inline  ::capnp::List< ::replay::Instruction>::Reader ThreadChunk::Reader::getInstructions() const {
  return ::capnp::_::PointerHelpers< ::capnp::List< ::replay::Instruction>>::get(_reader.getPointerField(
      ::capnp::bounded<0>() * ::capnp::POINTERS));
}
inline  ::capnp::List< ::replay::Instruction>::Builder ThreadChunk::Builder::getInstructions() {
  return ::capnp::_::PointerHelpers< ::capnp::List< ::replay::Instruction>>::get(_builder.getPointerField(
      ::capnp::bounded<0>() * ::capnp::POINTERS));
}
inline void ThreadChunk::Builder::setInstructions( ::capnp::List< ::replay::Instruction>::Reader value) {
  ::capnp::_::PointerHelpers< ::capnp::List< ::replay::Instruction>>::set(_builder.getPointerField(
      ::capnp::bounded<0>() * ::capnp::POINTERS), value);
}
inline  ::capnp::List< ::replay::Instruction>::Builder ThreadChunk::Builder::initInstructions(unsigned int size) {
  return ::capnp::_::PointerHelpers< ::capnp::List< ::replay::Instruction>>::init(_builder.getPointerField(
      ::capnp::bounded<0>() * ::capnp::POINTERS), size);
}
inline void ThreadChunk::Builder::adoptInstructions(
    ::capnp::Orphan< ::capnp::List< ::replay::Instruction>>&& value) {
  ::capnp::_::PointerHelpers< ::capnp::List< ::replay::Instruction>>::adopt(_builder.getPointerField(
      ::capnp::bounded<0>() * ::capnp::POINTERS), kj::mv(value));
}
inline ::capnp::Orphan< ::capnp::List< ::replay::Instruction>> ThreadChunk::Builder::disownInstructions() {
  return ::capnp::_::PointerHelpers< ::capnp::List< ::replay::Instruction>>::disown(_builder.getPointerField(
      ::capnp::bounded<0>() * ::capnp::POINTERS));
}

inline bool Batch::Reader::hasThreads() const {
  return !_reader.getPointerField(
      ::capnp::bounded<0>() * ::capnp::POINTERS).isNull();
}
inline bool Batch::Builder::hasThreads() {
  return !_builder.getPointerField(
      ::capnp::bounded<0>() * ::capnp::POINTERS).isNull();
}
inline  ::capnp::List< ::replay::ThreadChunk>::Reader Batch::Reader::getThreads() const {
  return ::capnp::_::PointerHelpers< ::capnp::List< ::replay::ThreadChunk>>::get(_reader.getPointerField(
      ::capnp::bounded<0>() * ::capnp::POINTERS));
}
inline  ::capnp::List< ::replay::ThreadChunk>::Builder Batch::Builder::getThreads() {
  return ::capnp::_::PointerHelpers< ::capnp::List< ::replay::ThreadChunk>>::get(_builder.getPointerField(
      ::capnp::bounded<0>() * ::capnp::POINTERS));
}
inline void Batch::Builder::setThreads( ::capnp::List< ::replay::ThreadChunk>::Reader value) {
  ::capnp::_::PointerHelpers< ::capnp::List< ::replay::ThreadChunk>>::set(_builder.getPointerField(
      ::capnp::bounded<0>() * ::capnp::POINTERS), value);
}
inline  ::capnp::List< ::replay::ThreadChunk>::Builder Batch::Builder::initThreads(unsigned int size) {
  return ::capnp::_::PointerHelpers< ::capnp::List< ::replay::ThreadChunk>>::init(_builder.getPointerField(
      ::capnp::bounded<0>() * ::capnp::POINTERS), size);
}
inline void Batch::Builder::adoptThreads(
    ::capnp::Orphan< ::capnp::List< ::replay::ThreadChunk>>&& value) {
  ::capnp::_::PointerHelpers< ::capnp::List< ::replay::ThreadChunk>>::adopt(_builder.getPointerField(
      ::capnp::bounded<0>() * ::capnp::POINTERS), kj::mv(value));
}
inline ::capnp::Orphan< ::capnp::List< ::replay::ThreadChunk>> Batch::Builder::disownThreads() {
  return ::capnp::_::PointerHelpers< ::capnp::List< ::replay::ThreadChunk>>::disown(_builder.getPointerField(
      ::capnp::bounded<0>() * ::capnp::POINTERS));
}

}  // namespace

#endif  // CAPNP_INCLUDED_cb49ade5c790d703_
