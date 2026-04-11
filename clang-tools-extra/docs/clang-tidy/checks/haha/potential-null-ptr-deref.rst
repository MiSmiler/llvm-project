.. title:: clang-tidy - haha-potential-null-ptr-deref

haha-potential-null-ptr-deref
==============================

Detects potential null pointer dereferences.

This check uses dataflow analysis to track pointer nullness across control flow
and warns when a pointer that may be null is dereferenced without a prior null
check.

Description
-----------

The check tracks pointer variables and determines whether they may be null at
each dereference point. It warns when:

1. A pointer is initialized to ``nullptr`` and dereferenced without a null check
2. A pointer returned from a function (without non-null annotation) is dereferenced directly
3. A pointer parameter is dereferenced without a prior null check
4. Smart pointers (``std::unique_ptr``, ``std::shared_ptr``) are dereferenced without checking

The check does NOT warn when:

1. The pointer has been checked for nullness in a preceding ``if`` statement
2. The pointer is initialized via ``new`` (which always returns non-null)
3. The pointer is returned from a function with ``_Nonnull`` or ``__nonnull`` attribute
4. The pointer comes from ``std::make_unique`` or ``std::make_shared``

Options
-------

None.

Examples
--------

.. code-block:: c++

  void foo() {
    int* p = nullptr;
    init(&p);  // May or may not initialize p

    // Warning: p may still be nullptr
    int x = *p;
  }

Safe code would check the pointer first:

.. code-block:: c++

  void foo() {
    int* p = nullptr;
    init(&p);

    if (p != nullptr) {
      int x = *p;  // OK - checked before use
    }
  }

The check also handles smart pointers:

.. code-block:: c++

  void bar() {
    std::unique_ptr<int> p(nullptr);

    // Warning: p may be nullptr
    int x = *p;

    if (p) {
      int y = *p;  // OK - checked before use
    }
  }