#pragma once

// ============================================================================
// JSEngine — PUENTE DE COMPATIBILIDAD.
//
// Aquí vivía el impostor: un "eval()" que partía el código por ';' y hacía
// coincidencias de texto con "console.log(" y "getElementById(". Eso NO era
// JavaScript, y ya no existe: esta clase ahora delega todo en el intérprete
// real (js/js_interp.hpp: lexer → parser recursivo → AST → evaluador).
// La API se conserva para no romper consumidores antiguos.
// ============================================================================

#include "js_interp.hpp"
#include "../html/document.hpp"
#include <string>
#include <memory>
#include <vector>

namespace nuby::js {

using JSValue = Value; // el valor real del intérprete reemplaza al struct viejo

class JSEngine {
public:
    explicit JSEngine(std::shared_ptr<html::Document> doc)
        : interp_(std::make_shared<Interpreter>(std::move(doc))) {}

    // Ejecuta código con el intérprete REAL. Lanza std::runtime_error
    // si el programa usa sintaxis fuera del subconjunto soportado.
    Value eval(const std::string& script) {
        interp_->run(script);
        return Value::undefined();
    }

    const std::vector<std::string>& get_console_logs() const { return interp_->logs(); }
    bool has_dom_mutated() const { return interp_->dom_mutated(); }
    void clear_mutation_flag() { interp_->clear_mutation(); }

    bool dispatch_click(const std::string& element_id) {
        return interp_->dispatch_click(element_id);
    }

    std::shared_ptr<Interpreter> interpreter() const { return interp_; }

private:
    std::shared_ptr<Interpreter> interp_;
};

} // namespace nuby::js
