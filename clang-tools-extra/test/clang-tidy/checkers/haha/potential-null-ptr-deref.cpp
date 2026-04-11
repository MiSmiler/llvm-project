// RUN: %check_clang_tidy %s haha-potential-null-ptr-deref %t

struct Info {
  std::string name;
};

void MayInitInfo(Info** p);
Info* make_builder();

// Test case 1: Pointer initialized to nullptr and dereferenced without check
void foo1() {
  Info* pInfo = nullptr;
  MayInitInfo(&pInfo);
  // Should warn: pInfo may still be nullptr
  std::string s = pInfo->name;
  // CHECK-MESSAGES: :[[@LINE-1]]:20: warning: potential null pointer dereference
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
    std::string s = pInfo->name;  // OK - checked before use
  }
}

// Test case 4: Another form of null check - no warning
void foo4() {
  Info* pInfo = nullptr;
  MayInitInfo(&pInfo);
  if (pInfo) {
    std::string s = pInfo->name;  // OK - implicit null check
  }
}

// Test case 5: Dereference after else - warning
void foo5() {
  Info* pInfo = nullptr;
  MayInitInfo(&pInfo);
  if (!pInfo) {
    return;
  }
  std::string s = pInfo->name;  // OK - early return ensures non-null
}

// Test case 6: new expression - non-null guaranteed, no warning
void foo6() {
  Info* pInfo = new Info();
  std::string s = pInfo->name;  // OK - new always returns non-null
}

// Test case 7: Pointer dereference with * operator
void foo7() {
  Info* pInfo = nullptr;
  Info& ref = *pInfo;  // Should warn
  // CHECK-MESSAGES: :[[@LINE-1]]:15: warning: potential null pointer dereference
}

// Test case 8: Smart pointer dereference
namespace std {
template<typename T>
class unique_ptr {
public:
  T* operator->() const noexcept;
  T& operator*() const noexcept;
  T* get() const noexcept;
};

template<typename T>
class shared_ptr {
public:
  T* operator->() const noexcept;
  T& operator*() const noexcept;
  T* get() const noexcept;
};
}

void foo8() {
  std::unique_ptr<Info> pInfo(nullptr);
  std::string s = pInfo->name;  // Should warn
  // CHECK-MESSAGES: :[[@LINE-1]]:20: warning: potential null pointer dereference
}

// Test case 9: Smart pointer with proper check
void foo9() {
  std::unique_ptr<Info> pInfo(nullptr);
  if (pInfo) {
    std::string s = pInfo->name;  // OK - checked before use
  }
}

// Test case 10: Unknown pointer parameter - warning in strict mode
void foo10(Info* pParam) {
  std::string s = pParam->name;  // Should warn - unknown pointer may be null
  // CHECK-MESSAGES: :[[@LINE-1]]:20: warning: potential null pointer dereference
}

// Test case 11: Pointer parameter with null check
void foo11(Info* pParam) {
  if (pParam) {
    std::string s = pParam->name;  // OK
  }
}