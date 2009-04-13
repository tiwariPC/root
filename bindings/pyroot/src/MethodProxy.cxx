// @(#)root/pyroot:$Id$
// Author: Wim Lavrijsen, Jan 2005

// Bindings
#include "PyROOT.h"
#include "structmember.h"    // from Python
#if PY_MAJOR_VERSION >= 2 && PY_MINOR_VERSION >= 5
#include "code.h"            // from Python
#else
#include "compile.h"         // from Python
#endif
#ifndef CO_NOFREE
// python2.2 does not have CO_NOFREE defined
#define CO_NOFREE       0x0040
#endif
#include "MethodProxy.h"
#include "ObjectProxy.h"
#include "TPyException.h"
#include "Utility.h"
#include "PyStrings.h"

// Standard
#include <algorithm>
#include <functional>
#include <vector>
#include <algorithm>


namespace PyROOT {

namespace {

// helper to test whether a method is used in a pseudo-function modus
   bool inline IsPseudoFunc( MethodProxy* pymeth )
   {
      return (void*)pymeth == (void*)pymeth->fSelf;
   }

// helper for collecting/maintaining exception data in overload dispatch
   struct PyError_t {
      PyError_t() { fType = fValue = fTrace = 0; }

      static void Clear( PyError_t& e )
      {
         Py_XDECREF( e.fType ); Py_XDECREF( e.fValue ); Py_XDECREF( e.fTrace );
         e.fType = e.fValue = e.fTrace = 0;
      }

      PyObject *fType, *fValue, *fTrace;
   };

// helper to hash tuple (using tuple hash would cause self-tailing loops)
   inline Long_t HashSignature( PyObject* args )
   {
      ULong_t hash = 0;

      Int_t nargs = PyTuple_GET_SIZE( args );
      for ( Int_t i = 0; i < nargs; ++i ) {
         hash += (ULong_t) PyTuple_GET_ITEM( args, i )->ob_type;
         hash += (hash << 10); hash ^= (hash >> 6);
      }

      hash += (hash << 3); hash ^= (hash >> 11); hash += (hash << 15);

      return hash;
   }

// helper to sort on method priority
   int PriorityCmp( PyCallable* left, PyCallable* right )
   {
      return left->GetPriority() > right->GetPriority();
   }

// helper to factor out return logic of mp_call
   inline PyObject* HandleReturn( MethodProxy* pymeth, PyObject* result ) {

   // special case for python exceptions, propagated through C++ layer
      if ( result == (PyObject*)TPyExceptionMagic )
         return 0;              // exception info was already set

   // if this is creates new objects, always take ownership
      if ( ( pymeth->fMethodInfo->fFlags & MethodProxy::MethodInfo_t::kIsCreator ) &&
           ObjectProxy_Check( result ) )
         ((ObjectProxy*)result)->HoldOn();

      return result;
   }


//= PyROOT method proxy object behaviour =====================================
   PyObject* mp_name( MethodProxy* pymeth, void* )
   {
      return PyString_FromString( pymeth->GetName().c_str() );
   }

//____________________________________________________________________________
   PyObject* mp_module( MethodProxy* /* pymeth */, void* )
   {
      Py_INCREF( PyStrings::gROOTns );
      return PyStrings::gROOTns;
   }

//____________________________________________________________________________
   PyObject* mp_doc( MethodProxy* pymeth, void* )
   {
      MethodProxy::Methods_t& methods = pymeth->fMethodInfo->fMethods;

   // collect doc strings
      Int_t nMethods = methods.size();
      PyObject* doc = methods[0]->GetDocString();

   // simple case
      if ( nMethods == 1 )
         return doc;

   // overloaded method
      PyObject* separator = PyString_FromString( "\n" );
      for ( Int_t i = 1; i < nMethods; ++i ) {
         PyString_Concat( &doc, separator );
         PyString_ConcatAndDel( &doc, methods[i]->GetDocString() );
      }
      Py_DECREF( separator );

      return doc;
   }

//____________________________________________________________________________
   PyObject* mp_meth_func( MethodProxy* pymeth, void* )
   {
   // create and a new method proxy to be returned
      MethodProxy* newPyMeth = (MethodProxy*)MethodProxy_Type.tp_alloc( &MethodProxy_Type, 0 );

   // method info is shared, as it contains the collected overload knowledge
      *pymeth->fMethodInfo->fRefCount += 1;
      newPyMeth->fMethodInfo = pymeth->fMethodInfo;

   // new method is unbound, use of 'meth' is for keeping track whether this
   // proxy is used in the capacity of a method or a function
      newPyMeth->fSelf = (ObjectProxy*)newPyMeth;

      return (PyObject*)newPyMeth;
   }

//____________________________________________________________________________
   PyObject* mp_meth_self( MethodProxy* pymeth, void* )
   {
   // return the bound self, if any; in case of pseudo-function role, pretend
   // that the data member im_self does not exist
      if ( IsPseudoFunc( pymeth ) ) {
         PyErr_Format( PyExc_AttributeError,
            "function %s has no attribute \'im_self\'", pymeth->fMethodInfo->fName.c_str() );
         return 0;
      } else if ( pymeth->fSelf != 0 ) {
         Py_INCREF( (PyObject*)pymeth->fSelf );
         return (PyObject*)pymeth->fSelf;
      }

      Py_INCREF( Py_None );
      return Py_None;
   }

//____________________________________________________________________________
   PyObject* mp_meth_class( MethodProxy* pymeth, void* )
   {
   // return scoping class; in case of pseudo-function role, pretend that there
   // is no encompassing class (i.e. global scope)
      if ( ! IsPseudoFunc( pymeth ) ) {
         PyObject* pyclass = pymeth->fMethodInfo->fMethods[0]->GetScope();
         if ( ! pyclass )
            PyErr_Format( PyExc_AttributeError,
               "function %s has no attribute \'im_class\'", pymeth->fMethodInfo->fName.c_str() );
         return pyclass;
      }

      Py_INCREF( Py_None );
      return Py_None;
   }

//____________________________________________________________________________
   PyObject* mp_func_closure( MethodProxy* /* pymeth */, void* )
   {
      Py_INCREF( Py_None );
      return Py_None;
   }

//____________________________________________________________________________
   PyObject* mp_func_code( MethodProxy* pymeth, void* )
   {
      MethodProxy::Methods_t& methods = pymeth->fMethodInfo->fMethods;

   // collect maximum number of arguments in set of overloads; this is also used
   // for the number of locals ("stack variables")
      int co_argcount = 0;
      MethodProxy::Methods_t::iterator maxargmeth;
      for ( MethodProxy::Methods_t::iterator imeth = methods.begin(); imeth != methods.end(); ++imeth ) {
         if ( co_argcount < (*imeth)->GetMaxArgs() ) {
            co_argcount = (*imeth)->GetMaxArgs();
            maxargmeth = imeth;
         }
      }
      co_argcount += 1;       // for 'self'

   // for now, code object representing the statement 'pass'
      PyObject* co_code = PyString_FromStringAndSize( "d\x00\x00S", 4 );
   //   PyObject* co_code = PyString_FromStringAndSize( "t\x00\x00d\x01\x00\x83\x01\x00}\x02\x00|\x02\x00S", 16 );

   // tuple with all the const literals used in the function
      PyObject* co_consts = PyTuple_New( 2 );
      Py_INCREF( Py_None );
      PyTuple_SET_ITEM( co_consts, 0, Py_None );
      PyObject* val1 = PyFloat_FromDouble( -1.0 );
      PyTuple_SET_ITEM( co_consts, 1, val1 );

      PyObject* co_names = PyTuple_New( 2 );
      PyTuple_SET_ITEM( co_names, 0, PyString_FromString( "dafunc" ) );
      PyTuple_SET_ITEM( co_names, 1, PyString_FromString( "acos" ) );


   // names, freevars, and cellvars go unused
      PyObject* co_unused = PyTuple_New( 0 );

   // variable names are both the argument and local names
      PyObject* co_varnames = PyTuple_New( co_argcount + 1 );
      PyTuple_SET_ITEM( co_varnames, 0, PyString_FromString( "self" ) );
      for ( int iarg = 1; iarg < co_argcount; ++iarg ) {
         PyTuple_SET_ITEM( co_varnames, iarg, (*maxargmeth)->GetArgSpec( iarg - 1 ) );
      }
      PyTuple_SET_ITEM( co_varnames, co_argcount, PyString_FromString( "d" ) );

   // filename is made-up
      PyObject* co_filename = PyString_FromString( "ROOT.py" );

   // name is the function name, also through __name__ on the function itself
      PyObject* co_name = PyString_FromString( pymeth->GetName().c_str() );

   // firstlineno is the line number of first function code in the containing scope

   // lnotab is a packed table that maps instruction count and line number
   //PyObject* co_lnotab = PyString_FromString( "\x00\x01" );
      PyObject* co_lnotab = PyString_FromString( "\x00\x01\x0c\x01" );

      PyObject* code = (PyObject*)PyCode_New(
         co_argcount,                             // argcount
         co_argcount + 1,                         // nlocals
         2,                                       // stacksize
         CO_OPTIMIZED | CO_NEWLOCALS | CO_NOFREE, // flags
         co_code,                                 // code
         co_consts,                               // consts
         co_names,                                // names
         co_varnames,                             // varnames
         co_unused,                               // freevars
         co_unused,                               // cellvars
         co_filename,                             // filename
         co_name,                                 // name
         1,                                       // firstlineno
         co_lnotab );                             // lnotab

      Py_DECREF( co_lnotab );
      Py_DECREF( co_name );
      Py_DECREF( co_unused );
      Py_DECREF( co_filename );
      Py_DECREF( co_varnames );
      Py_DECREF( co_names );
      Py_DECREF( co_consts );
      Py_DECREF( co_code );

      return code;
   }

//____________________________________________________________________________
   PyObject* mp_func_defaults( MethodProxy* pymeth, void* )
   {
   // create a tuple of default values for the overload with the most arguments
      MethodProxy::Methods_t& methods = pymeth->fMethodInfo->fMethods;

      int maxarg = 0;
      MethodProxy::Methods_t::iterator maxargmeth;
      for ( MethodProxy::Methods_t::iterator imeth = methods.begin(); imeth != methods.end(); ++imeth ) {
         if ( maxarg < (*imeth)->GetMaxArgs() ) {
            maxarg = (*imeth)->GetMaxArgs();
            maxargmeth = imeth;
         }
      }

      PyObject* defaults = PyTuple_New( maxarg );

      int itup = 0;
      for ( int iarg = 0; iarg < maxarg; ++iarg ) {
         PyObject* defvalue = (*maxargmeth)->GetArgDefault( iarg );
         if ( defvalue )
            PyTuple_SET_ITEM( defaults, itup++, defvalue );
      }
      _PyTuple_Resize( &defaults, itup );

      return defaults;
   }

//____________________________________________________________________________
   PyObject* mp_func_globals( MethodProxy* /* pymeth */, void* )
   {
   // could also use __main__'s dict here; used for lookup of names from co_code
   // indexing into co_names
      PyObject* pyglobal = PyModule_GetDict( PyImport_AddModule( (char*)"ROOT" ) );
      Py_XINCREF( pyglobal );
      return pyglobal;
   }

//____________________________________________________________________________
   PyObject* mp_getcreates( MethodProxy* pymeth, void* )
   {
      return PyInt_FromLong(
         (Bool_t)(pymeth->fMethodInfo->fFlags & MethodProxy::MethodInfo_t::kIsCreator ) );
   }

//____________________________________________________________________________
   int mp_setcreates( MethodProxy* pymeth, PyObject* value, void* )
   {
      Long_t iscreator = PyLong_AsLong( value );
      if ( iscreator == -1 && PyErr_Occurred() ) {
         PyErr_SetString( PyExc_ValueError, "a boolean 1 or 0 is required for _creates" );
         return -1;
      }

      if ( iscreator )
         pymeth->fMethodInfo->fFlags |= MethodProxy::MethodInfo_t::kIsCreator;
      else
         pymeth->fMethodInfo->fFlags &= ~MethodProxy::MethodInfo_t::kIsCreator;

      return 0;
   }

//____________________________________________________________________________
   PyGetSetDef mp_getset[] = {
      { (char*)"__name__",   (getter)mp_name,   NULL, NULL, NULL },
      { (char*)"__module__", (getter)mp_module, NULL, NULL, NULL },
      { (char*)"__doc__",    (getter)mp_doc,    NULL, NULL, NULL },

   // to be more python-like, where these are duplicated as well; to actually
   // derive from the python method or function type is too memory-expensive,
   // given that most of the members of those types would not be used
      { (char*)"im_func",  (getter)mp_meth_func,  NULL, NULL, NULL },
      { (char*)"im_self",  (getter)mp_meth_self,  NULL, NULL, NULL },
      { (char*)"im_class", (getter)mp_meth_class, NULL, NULL, NULL },

      { (char*)"func_closure",  (getter)mp_func_closure,  NULL, NULL, NULL },
      { (char*)"func_code",     (getter)mp_func_code,     NULL, NULL, NULL },
      { (char*)"func_defaults", (getter)mp_func_defaults, NULL, NULL, NULL },
      { (char*)"func_globals",  (getter)mp_func_globals,  NULL, NULL, NULL },
      { (char*)"func_doc",      (getter)mp_doc,           NULL, NULL, NULL },
      { (char*)"func_name",     (getter)mp_name,          NULL, NULL, NULL },

      { (char*)"_creates", (getter)mp_getcreates, (setter)mp_setcreates,
            (char*)"For ownership rules of result: if true, objects are python-owned", NULL },
      { (char*)NULL, NULL, NULL, NULL, NULL }
   };

//= PyROOT method proxy function behavior ====================================
   PyObject* mp_call( MethodProxy* pymeth, PyObject* args, PyObject* kwds )
   {
   // if called through im_func pseudo-representation (this can be gamed if the
   // user really wants to ... )
      if ( IsPseudoFunc( pymeth ) )
         pymeth->fSelf = NULL;

   // get local handles to proxy internals
      MethodProxy::Methods_t&     methods     = pymeth->fMethodInfo->fMethods;
      MethodProxy::DispatchMap_t& dispatchMap = pymeth->fMethodInfo->fDispatchMap;

      Int_t nMethods = methods.size();

   // simple case
      if ( nMethods == 1 )
         return HandleReturn( pymeth, (*methods[0])( pymeth->fSelf, args, kwds ) );

   // otherwise, handle overloading
      Long_t sighash = HashSignature( args );

   // look for known signatures ...
      MethodProxy::DispatchMap_t::iterator m = dispatchMap.find( sighash );
      if ( m != dispatchMap.end() ) {
         Int_t index = m->second;
         PyObject* result = HandleReturn( pymeth, (*methods[ index ])( pymeth->fSelf, args, kwds ) );
         if ( result != 0 )
            return result;

      // fall through: python is dynamic, and so, the hashing isn't infallible
         PyErr_Clear();
      }

   // ... otherwise loop over all methods and find the one that does not fail
      if ( ! ( pymeth->fMethodInfo->fFlags & MethodProxy::MethodInfo_t::kIsSorted ) ) {
         std::stable_sort( methods.begin(), methods.end(), PriorityCmp );
         pymeth->fMethodInfo->fFlags |= MethodProxy::MethodInfo_t::kIsSorted;
      }

      std::vector< PyError_t > errors;
      for ( Int_t i = 0; i < nMethods; ++i ) {
         PyObject* result = (*methods[i])( pymeth->fSelf, args, kwds );

         if ( result == (PyObject*)TPyExceptionMagic ) {
            std::for_each( errors.begin(), errors.end(), PyError_t::Clear );
            return 0;              // exception info was already set
         }

         if ( result != 0 ) {
         // success: update the dispatch map for subsequent calls
            dispatchMap[ sighash ] = i;
            std::for_each( errors.begin(), errors.end(), PyError_t::Clear );
            return HandleReturn( pymeth, result );
         }

      // failure: collect error message/trace (automatically clears exception, too)
         if ( ! PyErr_Occurred() ) {
         // this should not happen; set an error to prevent core dump and report
            PyObject* sig = methods[i]->GetPrototype();
            PyErr_Format( PyExc_SystemError, "%s =>\n    %s",
               PyString_AS_STRING( sig ), (char*)"NULL result without error in mp_call" );
            Py_DECREF( sig );
         }
         PyError_t e;
         PyErr_Fetch( &e.fType, &e.fValue, &e.fTrace );
         errors.push_back( e );
      }

   // first summarize, then add details
      PyObject* value = PyString_FromFormat(
         "none of the %d overloaded methods succeeded. Full details:", nMethods );
      PyObject* separator = PyString_FromString( "\n  " );

   // if this point is reached, none of the overloads succeeded: notify user
      for ( std::vector< PyError_t >::iterator e = errors.begin(); e != errors.end(); ++e ) {
         PyString_Concat( &value, separator );
         PyString_Concat( &value, e->fValue );
      }

      Py_DECREF( separator );
      std::for_each( errors.begin(), errors.end(), PyError_t::Clear );

   // report failure
      PyErr_SetObject( PyExc_TypeError, value );
      Py_DECREF( value );
      return 0;
   }

//____________________________________________________________________________
   MethodProxy* mp_descrget( MethodProxy* pymeth, ObjectProxy* pyobj, PyObject* )
   {
   // create and use a new method proxy (language requirement)
      MethodProxy* newPyMeth = (MethodProxy*)MethodProxy_Type.tp_alloc( &MethodProxy_Type, 0 );

   // method info is shared, as it contains the collected overload knowledge
      *pymeth->fMethodInfo->fRefCount += 1;
      newPyMeth->fMethodInfo = pymeth->fMethodInfo;

   // new method is to be bound to current object (may be NULL)
      Py_XINCREF( (PyObject*)pyobj );
      newPyMeth->fSelf = pyobj;

      return newPyMeth;
   }


//= PyROOT method proxy construction/destruction =================================
   MethodProxy* mp_new( PyTypeObject*, PyObject*, PyObject* )
   {
      MethodProxy* pymeth = PyObject_GC_New( MethodProxy, &MethodProxy_Type );
      pymeth->fSelf = NULL;
      pymeth->fMethodInfo = new MethodProxy::MethodInfo_t;

      PyObject_GC_Track( pymeth );
      return pymeth;
   }

//____________________________________________________________________________
   void mp_dealloc( MethodProxy* pymeth )
   {
      PyObject_GC_UnTrack( pymeth );

      if ( ! IsPseudoFunc( pymeth ) ) {
         Py_XDECREF( (PyObject*)pymeth->fSelf );
      }

      pymeth->fSelf = NULL;

      if ( --(*pymeth->fMethodInfo->fRefCount) <= 0 ) {
         delete pymeth->fMethodInfo;
      }
 
      PyObject_GC_Del( pymeth );
   }


//____________________________________________________________________________
   long mp_hash( MethodProxy* pymeth )
   {
   // with fMethodInfo shared, it's address is better suited for the hash
      return _Py_HashPointer( pymeth->fMethodInfo );
   }

//____________________________________________________________________________
   int mp_traverse( MethodProxy* pymeth, visitproc visit, void* args )
   {
      if ( pymeth->fSelf && ! IsPseudoFunc( pymeth ) )
         return visit( (PyObject*)pymeth->fSelf, args );

      return 0;
   }

//____________________________________________________________________________
   int mp_clear( MethodProxy* pymeth )
   {
      Py_XDECREF( (PyObject*)pymeth->fSelf );
      pymeth->fSelf = NULL;

      return 0;
   }

//____________________________________________________________________________
   PyObject* mp_richcompare( MethodProxy* self, MethodProxy* other, int op )
   {
      if ( op != Py_EQ )
         return PyType_Type.tp_richcompare( (PyObject*)self, (PyObject*)other, op );

   // defined by type + (shared) MethodInfo + bound self, with special case for fSelf (i.e. pseudo-function)
      if ( ( self->ob_type == other->ob_type && self->fMethodInfo == other->fMethodInfo ) && \
           ( ( IsPseudoFunc( self ) && IsPseudoFunc( other ) ) || self->fSelf == other->fSelf ) ) {
         Py_INCREF( Py_True );
         return Py_True;
      }
      Py_INCREF( Py_False );
      return Py_False;
   }


//= PyROOT method proxy access to internals =================================
   PyObject* mp_disp( MethodProxy* pymeth, PyObject* args, PyObject* )
   {
      PyObject* sigarg = 0;
      if ( ! PyArg_ParseTuple( args, const_cast< char* >( "S:disp" ), &sigarg ) )
         return 0;

      PyObject* sig1 = PyString_FromFormat( "(%s)", PyString_AS_STRING( sigarg ) );

      MethodProxy::Methods_t& methods = pymeth->fMethodInfo->fMethods;
      for ( Int_t i = 0; i < (Int_t)methods.size(); ++i ) {
         PyObject* sig2 = methods[ i ]->GetSignature();
         if ( PyObject_Compare( sig1, sig2 ) == 0 ) {
            Py_DECREF( sig2 );

            MethodProxy* newmeth = mp_new( NULL, NULL, NULL );
            MethodProxy::Methods_t vec; vec.push_back( methods[ i ] );
            newmeth->Set( pymeth->fMethodInfo->fName, vec );

            Py_DECREF( sig1 );
            return (PyObject*)newmeth;
         }

         Py_DECREF( sig2 );
      }

      Py_DECREF( sig1 );
      PyErr_Format( PyExc_LookupError, "signature \"%s\" not found", PyString_AS_STRING( sigarg ) );
      return 0;
   }

//____________________________________________________________________________
   PyMethodDef mp_methods[] = {
      { (char*)"disp", (PyCFunction)mp_disp, METH_VARARGS, (char*)"select overload for dispatch" },
      { (char*)NULL, NULL, 0, NULL }
   };

} // unnamed namespace


//= PyROOT method proxy type =================================================
PyTypeObject MethodProxy_Type = {
   PyObject_HEAD_INIT( &PyType_Type )
   0,                         // ob_size
   (char*)"ROOT.MethodProxy", // tp_name
   sizeof(MethodProxy),       // tp_basicsize
   0,                         // tp_itemsize
   (destructor)mp_dealloc,    // tp_dealloc
   0,                         // tp_print
   0,                         // tp_getattr
   0,                         // tp_setattr
   0,                         // tp_compare
   0,                         // tp_repr
   0,                         // tp_as_number
   0,                         // tp_as_sequence
   0,                         // tp_as_mapping
   (hashfunc)mp_hash,         // tp_hash
   (ternaryfunc)mp_call,      // tp_call
   0,                         // tp_str
   0,                         // tp_getattro
   0,                         // tp_setattro
   0,                         // tp_as_buffer
   Py_TPFLAGS_DEFAULT | Py_TPFLAGS_HAVE_GC,      // tp_flags
   (char*)"PyROOT method proxy (internal)",      // tp_doc
   (traverseproc)mp_traverse, // tp_traverse
   (inquiry)mp_clear,         // tp_clear
   (richcmpfunc)mp_richcompare,                  // tp_richcompare
   0,                         // tp_weaklistoffset
   0,                         // tp_iter
   0,                         // tp_iternext
   mp_methods,                // tp_methods
   0,                         // tp_members
   mp_getset,                 // tp_getset
   0,                         // tp_base
   0,                         // tp_dict
   (descrgetfunc)mp_descrget, // tp_descr_get
   0,                         // tp_descr_set
   0,                         // tp_dictoffset
   0,                         // tp_init
   0,                         // tp_alloc
   (newfunc)mp_new,           // tp_new
   0,                         // tp_free
   0,                         // tp_is_gc
   0,                         // tp_bases
   0,                         // tp_mro
   0,                         // tp_cache
   0,                         // tp_subclasses
   0                          // tp_weaklist
#if PY_MAJOR_VERSION >= 2 && PY_MINOR_VERSION >= 3
   , 0                        // tp_del
#endif
#if PY_MAJOR_VERSION >= 2 && PY_MINOR_VERSION >= 6
   , 0                        // tp_version_tag
#endif
};

} // namespace PyROOT


//- public members -----------------------------------------------------------
void PyROOT::MethodProxy::Set( const std::string& name, std::vector< PyCallable* >& methods )
{
// set method data
   fMethodInfo->fName = name;
   fMethodInfo->fMethods.swap( methods );
   fMethodInfo->fFlags &= ~MethodInfo_t::kIsSorted;

// special case, in heuristics mode also tag *Clone* methods as creators
   if ( Utility::gMemoryPolicy == Utility::kHeuristics && name.find( "Clone" ) != std::string::npos )
      fMethodInfo->fFlags |= MethodInfo_t::kIsCreator;
}

//____________________________________________________________________________
PyROOT::MethodProxy::MethodInfo_t::~MethodInfo_t()
{
   for ( Methods_t::iterator it = fMethods.begin(); it != fMethods.end(); ++it ) {
      delete *it;
   }
   fMethods.clear();
   delete fRefCount;
}
