#pragma once

// ============================================================================
// STUB DEL INTÉRPRETE JS PARA LA BUILD DE NAVEGADOR (WebAssembly)
//
// Por qué existe: el intérprete real (js/js_interp.hpp) es un evaluador de
// JavaScript con excepciones C++ (señales internas de return/break/continue).
// C++ para WebAssembly en esta cadena de herramientas NO tiene excepciones, y
// el navegador no las aporta. Para que el MOTOR (parser, CSS, layout,
// rasterizador, buscador y la interfaz del navegador) se ejecute igual en el
// navegador, esta build enlaza este stub: la API es idéntica, pero el código
// de las páginas no se ejecuta.
//
// Es una limitación REAL y se dice sin adornos: la build nativa ejecuta
// JavaScript de verdad; la build de navegador no. No simula nada: devuelve
// "no hubo mutaciones ni errores" y nada más.
// ============================================================================

#include "../html/document.hpp"
#include <memory>
#include <string>
#include <vector>

namespace nuby::js {

// Valor mínimo para la API (la build nativa usa el Value real del intérprete)
struct Value {
    static Value undefined() { return Value(); }
};

class Interpreter {
public:
    explicit Interpreter(std::shared_ptr<html::Document> doc) : doc_(std::move(doc)) {}

    void run(const std::string&) {}                       // no hay intérprete en wasm
    const std::vector<std::string>& logs() const { return logs_; }
    bool dom_mutated() const { return false; }
    void clear_mutation() {}
    bool dispatch_click(const std::string&) { return false; }
    std::shared_ptr<html::Document> document() const { return doc_; }

private:
    std::shared_ptr<html::Document> doc_;
    std::vector<std::string> logs_;
};

} // namespace nuby::js
