@0xe5f4176bd4f65e15;

$import "/capnp/c++.capnp".namespace("replay");

struct Malloc {
  reg @0 :UInt64;
  size @1 :UInt64;
}

struct Free {
  reg @0 :UInt64;
}

struct Memalign {
  reg @0 :UInt64;
  size @1 :UInt64;
  alignment @2 :UInt64;
}

struct Realloc {
  oldReg @0 :UInt64;
  newReg @1 :UInt64;
  size @2 :UInt64;
}

struct FreeSized {
  reg @0 :UInt64;
  size @1 :UInt64;
}

struct KillThread {
}

struct SwitchThread {
  threadID @0 :UInt64;
}

struct Instruction {
  union {
    malloc @0 :Malloc;
    free @1 :Free;
    memalign @2 :Memalign;
    realloc @3 :Realloc;
    freeSized @4 :FreeSized;
    killThread @5 :KillThread;
    switchThread @6 :SwitchThread;
  }
}

struct Batch {
  instructions @0 :List(Instruction);
}
