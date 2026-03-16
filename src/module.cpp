#include <pybind11/pybind11.h>
#include <functional>
#include <unordered_map>
#include "dshm/types/shared_object.h"

namespace py = pybind11;

using factory_fn = std::function<py::object(std::string, std::string)>;

static std::unordered_map<PyObject*, factory_fn> factory_registry;

template<typename T>
void register_dtype(py::module_ &m, const char* name) {
    py::class_<shared_object<T>> cls(m, name);
    cls.def("get",   [](const shared_object<T>& self) { return static_cast<T>(self); })
       .def("set",   [](shared_object<T>& self, T val) { self = val; })
       .def("addr",  &shared_object<T>::addr)
       .def("destroy", &shared_object<T>::destroy)
       .def("__eq__", [](const shared_object<T>& a, const shared_object<T>& b) { return a == b; });

    py::object dtype = m.attr(name);
    factory_registry[dtype.ptr()] =
        [](std::string heap, std::string name) {
            return py::cast(dshm_make_or_find<T>(heap, name), py::return_value_policy::move);
        };
}

PYBIND11_MODULE(dshmpy, m) {
    register_dtype<std::int8_t>(m,   "int8");
    register_dtype<std::int32_t>(m,  "int32");
    register_dtype<std::int64_t>(m,  "int64");

    register_dtype<std::uint8_t>(m,  "uint8");
    register_dtype<std::uint32_t>(m, "uint32");
    register_dtype<std::uint64_t>(m, "uint64");
    
    register_dtype<float>(m,    "float32");
    register_dtype<double>(m,   "float64");

    m.def("make_or_find", [](std::string heap, std::string name, py::object dtype) -> py::object {
        auto it = factory_registry.find(dtype.ptr());
        if (it == factory_registry.end()) {
            throw std::runtime_error("unsupported dtype");
        }
        return it->second(heap, name);
    });
}