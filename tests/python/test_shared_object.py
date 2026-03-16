import pytest
import dshmpy
import uuid


@pytest.fixture
def heap_name():
    return f"testHeap_{uuid.uuid4().hex}"


def assert_true(value: bool) -> None:
    assert value


# ── Allocation ────────────────────────────────────────────────────────────────


def test_make_or_find_creates_object(heap_name):
    obj = dshmpy.make_or_find(heap_name, "obj_create", dshmpy.int32)
    assert obj.addr() != 0
    obj.destroy()


def test_make_or_find_finds_existing(heap_name):
    obj = dshmpy.make_or_find(heap_name, "obj_find", dshmpy.int32)
    obj.set(42)

    found = dshmpy.make_or_find(heap_name, "obj_find", dshmpy.int32)
    assert found.addr() == obj.addr()
    assert found.get() == 42

    obj.destroy()


def test_destroy_clears_addr(heap_name):
    obj = dshmpy.make_or_find(heap_name, "obj_destroy", dshmpy.int32)
    assert obj.addr() != 0
    obj.destroy()
    assert obj.addr() == 0


def test_unsupported_dtype_raises(heap_name):
    with pytest.raises(RuntimeError, match="unsupported dtype"):
        dshmpy.make_or_find(heap_name, "obj_bad", str)


# ── Get / Set ─────────────────────────────────────────────────────────────────


def test_set_and_get(heap_name):
    obj = dshmpy.make_or_find(heap_name, "obj_set", dshmpy.int32)
    obj.set(99)
    assert obj.get() == 99
    obj.destroy()


def test_get_default_value(heap_name):
    obj = dshmpy.make_or_find(heap_name, "obj_default", dshmpy.int32)
    assert obj.get() == 0
    obj.destroy()


# ── Equality ──────────────────────────────────────────────────────────────────


def test_eq_shared_object(heap_name):
    a = dshmpy.make_or_find(heap_name, "obj_eq_a", dshmpy.int32)
    b = dshmpy.make_or_find(heap_name, "obj_eq_b", dshmpy.int32)
    a.set(10)
    b.set(10)
    assert_true(a == b)
    a.destroy()
    b.destroy()


def test_eq_raw_value(heap_name):
    obj = dshmpy.make_or_find(heap_name, "obj_eq_raw", dshmpy.int32)
    obj.set(55)
    is_equal = obj == 55
    obj.destroy()
    assert is_equal


def test_neq_raw_value(heap_name):
    obj = dshmpy.make_or_find(heap_name, "obj_neq_raw", dshmpy.int32)
    obj.set(55)
    assert_true(not (obj == 99))
    obj.destroy()


# ── Arithmetic ────────────────────────────────────────────────────────────────


def test_add(heap_name):
    obj = dshmpy.make_or_find(heap_name, "obj_add", dshmpy.int32)
    obj.set(10)
    assert_true(obj + 5 == 15)
    obj.destroy()


def test_radd(heap_name):
    obj = dshmpy.make_or_find(heap_name, "obj_radd", dshmpy.int32)
    obj.set(10)
    assert_true(5 + obj == 15)
    obj.destroy()


def test_iadd(heap_name):
    obj = dshmpy.make_or_find(heap_name, "obj_iadd", dshmpy.int32)
    obj.set(10)
    obj += 5
    assert obj.get() == 15
    obj.destroy()


def test_sub(heap_name):
    obj = dshmpy.make_or_find(heap_name, "obj_sub", dshmpy.int32)
    obj.set(10)
    assert_true(obj - 3 == 7)
    obj.destroy()


def test_rsub(heap_name):
    obj = dshmpy.make_or_find(heap_name, "obj_rsub", dshmpy.int32)
    obj.set(3)
    assert_true(10 - obj == 7)
    obj.destroy()


def test_isub(heap_name):
    obj = dshmpy.make_or_find(heap_name, "obj_isub", dshmpy.int32)
    obj.set(10)
    obj -= 3
    assert obj.get() == 7
    obj.destroy()


# ── Increment / Decrement ─────────────────────────────────────────────────────


def test_pre_inc(heap_name):
    obj = dshmpy.make_or_find(heap_name, "obj_preinc", dshmpy.int32)
    obj.set(10)
    obj.inc()
    assert obj.get() == 11
    obj.destroy()


def test_pre_dec(heap_name):
    obj = dshmpy.make_or_find(heap_name, "obj_predec", dshmpy.int32)
    obj.set(10)
    obj.dec()
    assert obj.get() == 9
    obj.destroy()


def test_post_inc(heap_name):
    obj = dshmpy.make_or_find(heap_name, "obj_postinc", dshmpy.int32)
    obj.set(10)
    old = obj.inc_post()
    assert old == 10
    assert obj.get() == 11
    obj.destroy()


def test_post_dec(heap_name):
    obj = dshmpy.make_or_find(heap_name, "obj_postdec", dshmpy.int32)
    obj.set(10)
    old = obj.dec_post()
    assert old == 10
    assert obj.get() == 9
    obj.destroy()


# ── All integer dtypes ────────────────────────────────────────────────────────


@pytest.mark.parametrize(
    "dtype",
    [
        dshmpy.int8,
        dshmpy.int32,
        dshmpy.int64,
        dshmpy.uint8,
        dshmpy.uint32,
        dshmpy.uint64,
    ],
)
def test_integral_dtypes_arithmetic(heap_name, dtype):
    obj = dshmpy.make_or_find(heap_name, f"obj_dtype_{dtype.__name__}", dtype)
    obj.set(10)
    obj += 5
    assert obj.get() == 15
    obj -= 3
    assert obj.get() == 12
    obj.inc()
    assert obj.get() == 13
    obj.dec()
    assert obj.get() == 12
    obj.destroy()


# ── Float dtypes ──────────────────────────────────────────────────────────────


@pytest.mark.parametrize("dtype", [dshmpy.float32, dshmpy.float64])
def test_float_dtypes_set_get(heap_name, dtype):
    obj = dshmpy.make_or_find(heap_name, f"obj_dtype_{dtype.__name__}", dtype)
    obj.set(3.14)
    assert abs(obj.get() - 3.14) < 1e-5
    obj.destroy()


@pytest.mark.parametrize("dtype", [dshmpy.float32, dshmpy.float64])
def test_float_dtypes_no_arithmetic(heap_name, dtype):
    obj = dshmpy.make_or_find(heap_name, f"obj_dtype_noop_{dtype.__name__}", dtype)
    assert not hasattr(obj, "inc")
    assert not hasattr(obj, "dec")
    assert not hasattr(obj, "__iadd__")
    obj.destroy()


# ── Repr / Str ────────────────────────────────────────────────────────────────


def test_repr(heap_name):
    obj = dshmpy.make_or_find(heap_name, "obj_repr", dshmpy.int32)
    obj.set(7)
    assert "addr=" in repr(obj)
    assert "value=7" in repr(obj)
    obj.destroy()


def test_str(heap_name):
    obj = dshmpy.make_or_find(heap_name, "obj_str", dshmpy.int32)
    obj.set(7)
    assert str(obj) == "7"
    obj.destroy()
