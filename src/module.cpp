#include <pybind11/pybind11.h>
#include <functional>
#include <unordered_map>
#include "dshm/types/shared_object.h"

using namespace dshm;
namespace py = pybind11;

using factory_fn = std::function<py::object(std::string, std::string)>;

static std::unordered_map<PyObject*, factory_fn> factory_registry;

template<typename T>
void register_common(py::class_<shared_object<T>>& cls) {
    cls.def("get", [](const shared_object<T>& self) { return static_cast<T>(self); },
            "Get the value of the shared object value as the pythonic type\n"
        )
       .def("set", [](shared_object<T>& self, T val) { self = val; },
            "Set the shared object value\n\n"
            ":param val: The value to be set\n"
            ":returns: None\n"
            ":rtype: None\n"
        )
       .def("addr", &shared_object<T>::addr,
            "Get the shared address of the shared object\n\n"
            ":returns: Shared address.\n"
            ":rtype: int\n"
        )
       .def("destroy", &shared_object<T>::destroy,
            "Remove the shared object from memory\n\n"
            ":returns: None\n"
            ":rtype: None\n"
        )
       .def("__repr__", 
            [](const shared_object<T>& self) {
                return "<shared_object addr=" + std::to_string(self.addr()) + " value=" + std::to_string(static_cast<T>(self)) + ">";
            }
        )
       .def("__str__", 
            [](const shared_object<T>& self) {
                return std::to_string(static_cast<T>(self));
            }
        )
       .def("__eq__", 
            [](const shared_object<T>& a, const shared_object<T>& b) {
                return a == b;
            }
        )
       .def("__eq__", 
            [](const shared_object<T>& a, T b) {
                return static_cast<T>(a) == b;
            }
        );
}

template<typename T>
void register_integral_ops(py::class_<shared_object<T>>& cls) {
    cls.def("__add__", [](shared_object<T>& self, T val)  { return self + val; })
       .def("__radd__", [](shared_object<T>& self, T val)  { return self + val; })
       .def("__iadd__", [](shared_object<T>& self, T val) -> shared_object<T>& {
           self += val; return self;
       }, py::return_value_policy::reference)
       .def("__sub__", [](shared_object<T>& self, T val)  { return self - val; })
       .def("__rsub__", [](shared_object<T>& self, T val)  { return val - static_cast<T>(self); })
       .def("__isub__", [](shared_object<T>& self, T val) -> shared_object<T>& {
           self -= val; return self;
       }, py::return_value_policy::reference)
       .def("inc", [](shared_object<T>& self) -> shared_object<T>& {
           ++self; return self;
       }, py::return_value_policy::reference)
       .def("dec", [](shared_object<T>& self) -> shared_object<T>& {
           --self; return self;
       }, py::return_value_policy::reference);
}

template<typename T>
void register_dtype(py::module_& m, const char* name) {
    py::class_<shared_object<T>> cls(m, name);
    register_common<T>(cls);

    if constexpr (std::is_integral_v<T>) {
        register_integral_ops<T>(cls);
    }

    py::object dtype = m.attr(name);
    factory_registry[dtype.ptr()] =
        [](std::string heap, std::string name) {
            return py::cast(make_or_find<T>(heap, name), py::return_value_policy::move);
        };
}

PYBIND11_MODULE(dshmpy, m) {
    register_dtype<std::int8_t>(m,   "int8");
    register_dtype<std::int32_t>(m,  "int32");
    register_dtype<std::int64_t>(m,  "int64");

    register_dtype<std::uint8_t>(m,  "uint8");
    register_dtype<std::uint32_t>(m, "uint32");
    register_dtype<std::uint64_t>(m, "uint64");

    register_dtype<float>(m,  "float32");
    register_dtype<double>(m, "float64");

    m.def("make_or_find", 
        [](std::string heap, std::string name, py::object dtype) -> py::object {
            auto it = factory_registry.find(dtype.ptr());
            if (it == factory_registry.end()) {
                throw std::runtime_error("unsupported dtype");
            }
            return it->second(heap, name);
        },
        "Create or find a shared object within a shared namespace.\n\n"
        ":param shared_heap: The shared heap to search or create in\n"
        ":type shared_heap: str\n"
        ":param name: Name of the shared object\n"
        ":type name: str\n"
        ":param dtype: One of the dshmpy types e.g. dshmpy.int32, dshmpy.float64\n"
        ":type dtype: type\n"
        ":returns: A shared object of the given dtype.\n"
        ":rtype: object\n",
        py::arg("shared_heap"),
        py::arg("name"),
        py::arg("dtype")
    );
}