# LibTorch Stable ABI

This note will eventually contain more details on how to use the APIs in torch/csrc/stable. For the moment, it contains a table of internal representations:
1. type in custom extension: type used within the end user custom library.
2. StableIValue representation: a stable conversion of the type to liaison between the user model vs libtorch.so in an ABI-stable manner.
3. type in libtorch: type used within libtorch.so (or any code binary locked with libtorch).
4. Schema Type: type as described by the schema, which we hail as the source of truth for both ATen ops in native_functions.yaml and for user defined custom operators registered to the dispatcher via TORCH_LIBRARY or torch.library.

|  type in custom extension    |   StableIValue representation   |   type in libtorch  |   Schema Type  |
| -------- | ------- | ------- | ------- |
| std::optional\<S> | if there is a value, raw bitwise copy into leading bytes of uint64_t of pointer to a new StableIValue representing S. if there is no value, nullptr. | std::optional\<T> | Type? |
| RAIIATH | raw bitwise copy of underlying AtenTensorHandle into leading bytes of uint64_t | at::Tensor |  Tensor |
| int32_t | raw bitwise copy into leading bytes of uint64_t | at::ScalarType | ScalarType |
| int32_t | raw bitwise copy into leading bytes of uint64_t | at::Layout | Layout |
| int32_t | raw bitwise copy into leading bytes of uint64_t | at::MemoryFormat | MemoryFormat |
| bool | raw bitwise copy into leading bytes of uint64_t | bool | bool |
| int64_t | raw bitwise copy into leading bytes of uint64_t | int64_t | int |
| double | raw bitwise copy into leading bytes of uint64_t | double | float |
| ? | ? | c10::Device | Device |
| ? | ? | c10::Stream | Stream |
| ? | ? | c10::complex<double> | complex |
| ? | ? | at::Scalar | Scalar |
| ? | ? | std::string/const char*/ivalue::ConstantString | str |
| ? | ? | at::Storage | Storage |
| ? | ? | at::Generator | Generator |
| ? | ? | c10::List\<T> | Type[] |
| ? | ? | ivalue::Tuple\<T> | (Type, ...) |
| ? | ? | c10::SymInt | SymInt |
| ? | ? | c10::SymFloat | SymFloat |
| ? | ? | c10::SymBool | SymBool |
| ? | ? | at::QScheme | QScheme |

Our confidently supported types are the ones in the table that have completed rows. For a limited set of use cases, we also implicitly support any literal type that is representable within 64 bits as StableIValues, as the default reinterpret_cast will succeed. You can work with StableIValue abstractions in your custom kernel for types such as c10::Device even if there is no standard defined representation of device in custom extensions. For example, a custom operator can take as argument a StableIValue device and directly pass it through to an aten operator with `aoti_torch_call_dispatcher`.


## How to use stack-based APIs

`aoti_torch_call_dispatcher` is what we consider a stack-based API because it takes as input a stack of StableIValues. Working with the dispatcher will likely bring you into proximity with stack-based APIs, so we are documenting some invariants:

1. The stack is populated left to right.
    a. For example, a stack representing arguments `arg0`, `arg1`, and `arg2` will have `arg0` at index 0, `arg1` at index 1, and `arg2` at index 2.
    b. Returns are also populated left to right, e.g., `ret0` will be at index 0 and `ret1` will be at index 1, and so on.

2. The receiver of the stack consumes the stack and will "pop" all inputs off the stack, stealing ownership of any references.
    a. This is why when the stack is repopulated with returns, the returns start at index 0.

3. The stack always has ownership of the objects it holds.
    a. As the receiver of the stack is responsible for popping the stack, the stack must have ownership in order to transfer the ownership.
    b. As a corollary, the populater of the stack is responsible for ensuring that the stack has ownership of its underlying objects. In other words, the stack must not contain borrowed references.
