// RUN: %check_clang_tidy %s haha-potential-null-ptr-deref %t

struct Info {
  int index;
  int process() { return 0; }
};

void MayInitInfo(Info** p);
Info* make_builder();

// Test case 1: Pointer initialized to nullptr and dereferenced without check
void foo1() {
  Info* pInfo = nullptr;
  MayInitInfo(&pInfo);
  // Should warn: pInfo may still be nullptr
  int s = pInfo->index;
  // CHECK-MESSAGES: :[[@LINE-1]]:11: warning: potential null pointer dereference
}

void foo1_1() {
  Info* pInfo = nullptr;
  MayInitInfo(&pInfo);
  // Should warn: pInfo may still be nullptr
  int* s = &pInfo->index;
  // CHECK-MESSAGES: :[[@LINE-1]]:13: warning: potential null pointer dereference
  if (s)
    *s = 100;
}

// Test case 2: Function returning pointer dereferenced directly
void foo2() {
  // Should warn: make_builder() may return nullptr
  int err = make_builder()->process();
  // CHECK-MESSAGES: :[[@LINE-1]]:13: warning: potential null pointer dereference
}

// Test case 3: Proper null check before dereference - no warning
void foo3() {
  Info* pInfo = nullptr;
  MayInitInfo(&pInfo);
  if (pInfo != nullptr) {
    int s = pInfo->index;  // OK - checked before use
  }
}

// Test case 4: Another form of null check - no warning
void foo4() {
  Info* pInfo = nullptr;
  MayInitInfo(&pInfo);
  if (pInfo) {
    int s = pInfo->index;  // OK - implicit null check
  }
}

// Test case 5: Dereference after early return - no warning
void foo5() {
  Info* pInfo = nullptr;
  MayInitInfo(&pInfo);
  if (!pInfo) {
    return;
  }
  int s = pInfo->index;  // OK - early return ensures non-null
}

// Test case 6: new expression - non-null guaranteed, no warning
void foo6() {
  Info* pInfo = new Info();
  int s = pInfo->index;  // OK - new always returns non-null
}

// Test case 7: Pointer dereference with * operator
void foo7() {
  Info* pInfo = nullptr;
  Info& ref = *pInfo;  // Should warn
  // CHECK-MESSAGES: :[[@LINE-1]]:16: warning: potential null pointer dereference
}

// Test case 8: Smart pointer dereference
//
// NOTE: We define simplified unique_ptr/shared_ptr instead of #include <memory> because:
// 1. Test isolation: #include <memory> brings complex template code that may trigger other
//    checkers or unexpected warnings, interfering with precise test verification.
// 2. Simplified test environment: Custom version only declares minimal interfaces needed:
//    operator->, operator*, get(), operator bool. This is sufficient for clang-tidy to
//    recognize smart pointer patterns.
// 3. Avoid system header dependencies: #include <memory> introduces heavy template
//    metaprogramming, type traits, and other dependencies that may cause platform-specific
//    test inconsistencies.
// 4. LLVM/Clang testing convention: This simplified approach is standard practice in
//    clang-tidy official tests (see llvm-project/clang-tools-extra/test/clang-tidy/checkers/).
namespace std {
typedef decltype(nullptr) nullptr_t;

template<typename T>
class unique_ptr {
public:
  unique_ptr() noexcept : ptr_(nullptr) {}
  unique_ptr(nullptr_t) noexcept : ptr_(nullptr) {}
  T* operator->() const noexcept;
  T& operator*() const noexcept;
  T* get() const noexcept;
  explicit operator bool() const noexcept { return ptr_ != nullptr; }
private:
  T* ptr_;
};

template<typename T>
class shared_ptr {
public:
  shared_ptr() noexcept : ptr_(nullptr) {}
  shared_ptr(nullptr_t) noexcept : ptr_(nullptr) {}
  T* operator->() const noexcept;
  T& operator*() const noexcept;
  T* get() const noexcept;
  explicit operator bool() const noexcept { return ptr_ != nullptr; }
private:
  T* ptr_;
};
}

void foo8() {
  std::unique_ptr<Info> pInfo(nullptr);
  int s = pInfo->index;  // Should warn
  // CHECK-MESSAGES: :[[@LINE-1]]:11: warning: potential null pointer dereference
}

// Test case 9: Smart pointer with proper check
void foo9() {
  std::unique_ptr<Info> pInfo(nullptr);
  if (pInfo) {
    int s = pInfo->index;  // OK - checked before use
  }
}

// Test case 10: Unknown pointer parameter - warning in strict mode
void foo10(Info* pParam) {
  int s = pParam->index;  // Should warn - unknown pointer may be null
  // CHECK-MESSAGES: :[[@LINE-1]]:11: warning: potential null pointer dereference
}

// Test case 11: Pointer parameter with null check
void foo11(Info* pParam) {
  if (pParam) {
    int s = pParam->index;  // OK
  }
}

// Test case 12: Class method call - this pointer should never be warned
class MyClass {
public:
  int value;

  void helperMethod() {}

  // Test case 12.1: Explicit this-> member access - no warning
  void testExplicitThis() {
    int v = this->value;  // OK - this is never null in non-static member functions
  }

  // Test case 12.2: Implicit this member access - no warning
  void testImplicitThis() {
    int v = value;  // OK - implicit this is never null
  }

  // Test case 12.3: Explicit this-> method call - no warning
  void testExplicitThisMethod() {
    this->helperMethod();  // OK - this is never null
  }

  // Test case 12.4: Implicit this method call - no warning
  void testImplicitThisMethod() {
    helperMethod();  // OK - implicit this
  }

  // Test case 12.5: Dereference *this - no warning
  MyClass& getRef() {
    return *this;  // OK - this is never null
  }
};

// Test case 13: Calling method on potentially null pointer - should warn
void foo13(MyClass* p) {
  p->helperMethod();  // Should warn - p may be null
  // CHECK-MESSAGES: :[[@LINE-1]]:3: warning: potential null pointer dereference
}

// Test case 14: Calling method on pointer with null check - no warning
void foo14(MyClass* p) {
  if (p) {
    p->helperMethod();  // OK - p is checked
  }
}

// Test case 15: Accessing member on potentially null pointer - should warn
void foo15(MyClass* p) {
  int v = p->value;  // Should warn - p may be null
  // CHECK-MESSAGES: :[[@LINE-1]]:11: warning: potential null pointer dereference
}

// Test case 16: Accessing member on pointer with null check - no warning
void foo16(MyClass* p) {
  if (p != nullptr) {
    int v = p->value;  // OK - p is checked
  }
}

// Test case 17: Smart pointer operator* dereference - should warn
void foo17() {
  std::unique_ptr<Info> pInfo(nullptr);
  Info& ref = *pInfo;  // Should warn - smart pointer may be null
  // CHECK-MESSAGES: :[[@LINE-1]]:16: warning: potential null pointer dereference
}

// Test case 18: Smart pointer operator* with null check - no warning
void foo18() {
  std::unique_ptr<Info> pInfo(nullptr);
  if (pInfo) {
    Info& ref = *pInfo;  // OK - checked before use
  }
}

// Test case 19: Smart pointer as function parameter - should warn
void foo19(std::unique_ptr<Info> pParam) {
  int s = pParam->index;  // Should warn - unknown smart pointer may be null
  // CHECK-MESSAGES: :[[@LINE-1]]:11: warning: potential null pointer dereference
}

// Test case 20: Smart pointer parameter with null check - no warning
void foo20(std::unique_ptr<Info> pParam) {
  if (pParam) {
    int s = pParam->index;  // OK - checked before use
  }
}

// Test case 21: Early return with p == nullptr check - no warning after return
void foo21(Info* p) {
  if (p == nullptr) {
    return;
  }
  int s = p->index;  // OK - early return ensures non-null after the check
}

// Test case 22: Pointer dereference on function return value with * operator
void foo22() {
  Info& ref = *make_builder();  // Should warn - make_builder may return nullptr
  // CHECK-MESSAGES: :[[@LINE-1]]:16: warning: potential null pointer dereference
}

// Test case 23: shared_ptr operator-> dereference
void foo23() {
  std::shared_ptr<Info> pInfo(nullptr);
  int s = pInfo->index;  // Should warn
  // CHECK-MESSAGES: :[[@LINE-1]]:11: warning: potential null pointer dereference
}

// Test case 24: shared_ptr operator* dereference
void foo24() {
  std::shared_ptr<Info> pInfo(nullptr);
  Info& ref = *pInfo;  // Should warn
  // CHECK-MESSAGES: :[[@LINE-1]]:16: warning: potential null pointer dereference
}

// Test case 25: shared_ptr with null check - no warning
void foo25() {
  std::shared_ptr<Info> pInfo(nullptr);
  if (pInfo) {
    int s = pInfo->index;  // OK
  }
}

// Test case 26: shared_ptr as function parameter - should warn
void foo26(std::shared_ptr<Info> pParam) {
  int s = pParam->index;  // Should warn - unknown smart pointer may be null
  // CHECK-MESSAGES: :[[@LINE-1]]:11: warning: potential null pointer dereference
}

// Test case 27: shared_ptr parameter with null check - no warning
void foo27(std::shared_ptr<Info> pParam) {
  if (pParam) {
    int s = pParam->index;  // OK - checked before use
  }
}

// Test case 28: Smart pointer negation check - !pInfo pattern
void foo28() {
  std::unique_ptr<Info> pInfo(nullptr);
  if (!pInfo) {
    return;
  }
  int s = pInfo->index;  // OK - early return ensures non-null
}

// Test case 29: Raw pointer negation check - !p pattern
void foo29(Info* p) {
  if (!p) {
    return;
  }
  int s = p->index;  // OK - early return ensures non-null
}