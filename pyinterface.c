#include "rc522c.h"
#define PY_SSIZE_T_CLEAN
#include <Python.h>
#include <string.h>

static PyObject* RC522Error;
static PyObject* RC522TagError;

struct rc522
{
    PyObject_HEAD;
    struct rc522c_state cstate;
};

static void _raise_error(struct rc522c_state* cstate, enum rc522c_status status)
{
    switch (status)
    {
        break;
    case RC522C_STATUS_ERROR_PIGPIO:
        PyErr_Format(RC522Error, "pigpio error: code %d (rc522c.c:%d)", cstate->error_code, cstate->error_line);
        break;
    case RC522C_STATUS_ERROR_DEV_CMD_FAILED:
        PyErr_Format(
            RC522Error, "device command failed with error %d (rc522c.c:%d)", cstate->error_code, cstate->error_line);
        break;
    case RC522C_STATUS_ERROR_DEV_NOT_RESPONDING:
        PyErr_Format(RC522Error, "device does not respond to commands");
        break;
    case RC522C_STATUS_ERROR_TAG_MISSING:
        PyErr_Format(RC522TagError, "no response from the tag (rc522c.c:%d)", cstate->error_line);
        break;
    case RC522C_STATUS_ERROR_TAG_UNSUPPORTED:
        PyErr_Format(RC522TagError, "unsupported tag (rc522c.c:%d)", cstate->error_line);
        break;
    case RC522C_STATUS_ERROR_TAG_NAK: {
        switch (cstate->error_code)
        {
        case NTAG_NAK_INVALID_ARG:
            PyErr_Format(RC522TagError, "NAK: invalid command argument (rc522c.c:%d)", cstate->error_line);
            break;
        case NTAG_NAK_CRC_ERROR:
            PyErr_Format(RC522TagError, "NAK: parity or CRC error (rc522c.c:%d)", cstate->error_line);
            break;
        case NTAG_NAK_AUTH_CTR_OVERLOW:
            PyErr_Format(RC522TagError, "NAK: authentication counter overflow (rc522c.c:%d)", cstate->error_line);
            break;
        case NTAG_NAK_WRITE_ERROR:
            PyErr_Format(RC522TagError, "NAK: write error (rc522c.c:%d)", cstate->error_line);
            break;
        default:
            PyErr_Format(RC522TagError, "NAK: %d (rc522c.c:%d)", cstate->error_code, cstate->error_line);
            break;
        }
        break;
    }
    default:
        PyErr_Format(
            RC522Error,
            "unhandled status code %d in Python interface (internal error code %d, encountered at rc522c.c:%d)",
            (int)status, cstate->error_code, cstate->error_line);
        break;
    }
}

static PyObject* rc522_new(
    PyTypeObject* type, __attribute__((unused)) PyObject* args, __attribute__((unused)) PyObject* kwargs)
{
    struct rc522* self = (struct rc522*)type->tp_alloc(type, 0);
    return (PyObject*)self;
}

static int rc522_init(struct rc522* self, PyObject* args, PyObject* kwargs)
{
    static char* kwlist[] = {"spi_baud_rate", "antenna_gain", "rst_pin", NULL};
    int spi_baud_rate, antenna_gain, rst_pin;

    if (!PyArg_ParseTupleAndKeywords(args, kwargs, "iii", kwlist, &spi_baud_rate, &antenna_gain, &rst_pin))
        return -1;

    if (antenna_gain < 0 || antenna_gain > 7)
    {
        PyErr_Format(
            RC522Error,
            "Invalid antenna_gain value %d: supported values are 0...7. See the MFRC522 datasheet, section 9.3.3.6",
            antenna_gain);
        return -1;
    }

    enum rc522c_status status = rc522c_init(&self->cstate, spi_baud_rate, antenna_gain, rst_pin);
    if (status != RC522C_STATUS_SUCCESS)
    {
        _raise_error(&self->cstate, status);
        return -1;
    }

    return 0;
}

static void rc522_dealloc(struct rc522* self)
{
    rc522c_deinit(&self->cstate);
}

static PyObject* rc522_ntag_try_select(struct rc522* self, PyObject* Py_UNUSED(ignored))
{
    enum rc522c_status status = rc522c_ntag_select(&self->cstate);
    switch (status)
    {
    case RC522C_STATUS_SUCCESS:
        Py_RETURN_TRUE;
    case RC522C_STATUS_ERROR_TAG_MISSING:
    case RC522C_STATUS_ERROR_TAG_UNSUPPORTED:
        Py_RETURN_FALSE;
    default:
        _raise_error(&self->cstate, status);
        return NULL;
    }
}

static PyObject* rc522_ntag_read(struct rc522* self, PyObject* args)
{
    int from_page;
    if (!PyArg_ParseTuple(args, "i", &from_page))
        return NULL;

    char data[RC522_READ_LEN];
    enum rc522c_status status = rc522c_ntag_read(&self->cstate, from_page, data);
    if (status != RC522C_STATUS_SUCCESS)
    {
        _raise_error(&self->cstate, status);
        return NULL;
    }

    return PyBytes_FromStringAndSize(data, RC522_READ_LEN);
}

static PyObject* rc522_ntag_write(struct rc522* self, PyObject* args)
{
    int page;
    const char* data;
    Py_ssize_t data_len;
    if (!PyArg_ParseTuple(args, "is#", &page, &data, &data_len))
        return NULL;

    if (data_len != RC522_WRITE_LEN)
    {
        PyErr_SetString(PyExc_ValueError, "write command takes 4 bytes (1 page) of data at a time");
        return NULL;
    }

    enum rc522c_status status = rc522c_ntag_write(&self->cstate, page, data);
    if (status != RC522C_STATUS_SUCCESS)
    {
        _raise_error(&self->cstate, status);
        return NULL;
    }

    Py_RETURN_NONE;
}

static PyObject* rc522_ntag_authenticate(struct rc522* self, PyObject* args)
{
    const char* pwd;
    Py_ssize_t pwd_len;
    if (!PyArg_ParseTuple(args, "s#", &pwd, &pwd_len))
        return NULL;

    if (pwd_len != RC522_PWD_LEN)
    {
        PyErr_SetString(PyExc_ValueError, "password is required to be 4 bytes long");
        return NULL;
    }

    char pack[RC522_PACK_LEN];
    enum rc522c_status status = rc522c_ntag_authenticate(&self->cstate, pwd, pack);
    if (status != RC522C_STATUS_SUCCESS)
    {
        _raise_error(&self->cstate, status);
        return NULL;
    }

    return PyBytes_FromStringAndSize(pack, RC522_PACK_LEN);
}

static PyObject* rc522_ntag_protect(struct rc522* self, PyObject* args, PyObject* kwargs)
{
    const char* pwd;
    const char* pack;
    const char* mode;
    Py_ssize_t pwd_len, pack_len;
    int start_page;

    static char* kwlist[] = {"pwd", "pack", "start_page", "mode", NULL};
    if (!PyArg_ParseTupleAndKeywords(
            args, kwargs, "s#s#is", kwlist, &pwd, &pwd_len, &pack, &pack_len, &start_page, &mode))
        return NULL;

    if (pwd_len != RC522_PWD_LEN)
    {
        PyErr_SetString(PyExc_ValueError, "password is required to be 4 bytes long");
        return NULL;
    }
    if (pack_len != RC522_PACK_LEN)
    {
        PyErr_SetString(PyExc_ValueError, "PACK is required to be 2 bytes long");
        return NULL;
    }

    int rw;
    if (strcmp(mode, "w") == 0)
    {
        rw = 0;
    }
    else if (strcmp(mode, "rw") == 0)
    {
        rw = 1;
    }
    else
    {
        PyErr_SetString(
            PyExc_ValueError,
            "mode can be either 'w' (protect write access) or 'rw' (protect both read and write access)");
        return NULL;
    }

    enum rc522c_status status = rc522c_ntag_protect(&self->cstate, pwd, pack, start_page, rw);
    if (status != RC522C_STATUS_SUCCESS)
    {
        _raise_error(&self->cstate, status);
        return NULL;
    }

    Py_RETURN_NONE;
}

static PyObject* RC522_get_dev_version(struct rc522* self, __attribute__((unused)) void* closure)
{
    return PyLong_FromLong(self->cstate.dev_version);
}

static PyObject* RC522_get_tag_nfcid(struct rc522* self, __attribute__((unused)) void* closure)
{
    if (self->cstate.tag_selected)
        return PyBytes_FromStringAndSize(self->cstate.tag_nfcid, NTAG_NFCID_LEN);

    Py_RETURN_NONE;
}

static PyObject* RC522_get_tag_kind(struct rc522* self, __attribute__((unused)) void* closure)
{
    if (self->cstate.tag_selected)
    {
        switch (self->cstate.tag_kind)
        {
        case RC522C_TAG_KIND_213:
            return PyUnicode_FromString("NTAG213");
        case RC522C_TAG_KIND_215:
            return PyUnicode_FromString("NTAG215");
        case RC522C_TAG_KIND_216:
            return PyUnicode_FromString("NTAG216");
        case RC522C_TAG_KIND_UNKNOWN:
            Py_RETURN_NONE;
        }
    }

    Py_RETURN_NONE;
}

PyMODINIT_FUNC PyInit_rc522pi(void)
{
    static PyMethodDef rc522_methods[] = {
        {"ntag_try_select", (PyCFunction)rc522_ntag_try_select, METH_NOARGS, "TODO"},
        {"ntag_read", (PyCFunction)rc522_ntag_read, METH_VARARGS, "TODO"},
        {"ntag_write", (PyCFunction)rc522_ntag_write, METH_VARARGS, "TODO"},
        {"ntag_authenticate", (PyCFunction)rc522_ntag_authenticate, METH_VARARGS, "TODO"},
        {"ntag_protect", (PyCFunction)rc522_ntag_protect, METH_VARARGS | METH_KEYWORDS, "TODO"},
        {NULL}};

    static PyGetSetDef rc522_getset[] = {
        {"dev_version", (getter)RC522_get_dev_version, NULL, "TODO", NULL},
        {"tag_nfcid", (getter)RC522_get_tag_nfcid, NULL, "TODO", NULL},
        {"tag_kind", (getter)RC522_get_tag_kind, NULL, "TODO", NULL},
        {NULL}};

    static PyTypeObject rc522_type = {
        PyVarObject_HEAD_INIT(NULL, 0)

            .tp_name = "rc522pi.RC522",
        .tp_doc = "RC522 interface",
        .tp_basicsize = sizeof(struct rc522),
        .tp_itemsize = 0,
        .tp_flags = Py_TPFLAGS_DEFAULT,
        .tp_new = rc522_new,
        .tp_init = (initproc)rc522_init,
        .tp_dealloc = (destructor)rc522_dealloc,
        .tp_getset = rc522_getset,
        .tp_methods = rc522_methods};

    static PyMethodDef module_methods[] = {{NULL}};

    static struct PyModuleDef module_def = {
        PyModuleDef_HEAD_INIT,
        "rc522pi",
        "This module provides a pigpio-based interface for RC522 SPI RFID",
        0, /* no global state, no per-module state */
        module_methods,
        NULL, /* no multi-phase initialization */
        NULL, /* no gc traversal fun */
        NULL, /* no gc clearing fun */
        NULL  /* no deallocation fun */
    };

    if (PyType_Ready(&rc522_type) < 0)
        return NULL;

    PyObject* module = PyModule_Create(&module_def);
    if (!module)
        return NULL;

    Py_INCREF(&rc522_type);
    PyModule_AddObject(module, "RC522", (PyObject*)&rc522_type);

    RC522Error = PyErr_NewExceptionWithDoc(
        "rc522pi.RC522Error",
        "This exception indicates that a generic RC522 IO error has occurred (including tag errors).", NULL, NULL);
    if (!RC522Error)
        return NULL;
    PyModule_AddObject(module, "RC522Error", RC522Error);

    RC522TagError = PyErr_NewExceptionWithDoc(
        "rc522pi.RC522TagError",
        "This exception indicates that the tag command completed with either a NAK or an unexpected response.",
        RC522Error, NULL);
    if (!RC522TagError)
        return NULL;
    PyModule_AddObject(module, "RC522TagError", RC522TagError);

    return module;
}
