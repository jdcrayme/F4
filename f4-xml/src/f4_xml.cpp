// f4-xml/src/f4_xml.cpp
//
// f4-xml has no implementation of its own — the only translation unit is
// the vendored third_party/pugixml/pugixml.cpp compiled into the target.
// This file exists so the library keeps the same src/ layout as f4-json
// and f4-messaging (and so future shared XML diagnostics could live here
// without restructuring).

#include <f4/xml/f4_xml.hpp>
