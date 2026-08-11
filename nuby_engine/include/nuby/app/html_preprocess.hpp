#pragma once

// ============================================================================
// NUBY HTML PREPROCESS — Preparación de documentos reales de la web.
//
// La web real no es el HTML limpio de las demos. Esta etapa hace trabajo
// genuino antes del parser:
//   • Extrae bloques <style>…</style> y <script>…</script> a nivel de texto
//     (modo "raw text": su contenido puede contener '<' sin romper nada)
//   • <img> pasa real al DOM (el navegador la descarga y decodifica con el
//     decodificador PNG propio); <svg>, <video>, <audio>, <iframe> siguen
//     como placeholders honestos mientras no haya decodificador propio
//   • <input>, <textarea>, <button> → pasan REALES al DOM: el motor los
//     pinta, enfoca, edita y envía (GET y POST) sin simulación
//   • Entities HTML reales: &aacute; &#241; &#xF1; &nbsp; etc. → UTF-8
//   • <base href> respetado para resolver enlaces relativos
//   • Strip de @media/@import/@keyframes/@font-face en CSS externo (el
//     parser CSS no los soporta aún; quitarlos es la conducta honesta)
// ============================================================================

#include "../net/fetcher.hpp"
#include "../core/string_utils.hpp"
#include <string>
#include <vector>
#include <sstream>
#include <cctype>

namespace nuby::preprocess {

struct PreparedPage {
    std::string body_html;                 // HTML limpio para el parser
    std::string inline_css;                // CSS de los <style> de la página
    std::vector<std::string> inline_scripts;
    std::vector<std::string> external_scripts; // URLs absolutas de <script src>
    std::vector<std::string> external_css; // URLs absolutas de <link rel=stylesheet>
    std::string base_href;                 // <base href> si existe
};

// --- insensible a mayúsculas ------------------------------------------------
static inline bool ieq_at(const std::string& s, size_t pos, const char* lit) {
    for (size_t i = 0; lit[i]; ++i) {
        if (pos + i >= s.size()) return false;
        if (std::tolower((unsigned char)s[pos + i]) != std::tolower((unsigned char)lit[i])) return false;
    }
    return true;
}

// Encuentra el cierre </tag ignorando mayúsc/minúsculas
static size_t find_close(const std::string& s, size_t from, const char* tag) {
    std::string close = "</";
    close += tag;
    for (size_t i = from; i + close.size() <= s.size(); ++i) {
        if (ieq_at(s, i, close.c_str())) return i;
    }
    return std::string::npos;
}

// --- entities HTML (subconjunto real y útil para la web en español) ---------
static void push_utf8(std::string& out, uint32_t cp) {
    if (cp < 0x80) { out += (char)cp; return; }
    if (cp < 0x800) { out += (char)(0xC0 | (cp >> 6)); out += (char)(0x80 | (cp & 0x3F)); return; }
    out += (char)(0xE0 | (cp >> 12)); out += (char)(0x80 | ((cp >> 6) & 0x3F)); out += (char)(0x80 | (cp & 0x3F));
}

static std::string decode_entities(const std::string& in) {
    static const std::vector<std::pair<const char*, const char*>> named = {
        {"amp","&"},{"lt","<"},{"gt",">"},{"quot","\""},{"apos","'"},{"nbsp","\xC2\xA0"},
        {"aacute","á"},{"eacute","é"},{"iacute","í"},{"oacute","ó"},{"uacute","ú"},
        {"Aacute","Á"},{"Eacute","É"},{"Iacute","Í"},{"Oacute","Ó"},{"Uacute","Ú"},
        {"ntilde","ñ"},{"Ntilde","Ñ"},{"uuml","ü"},{"Uuml","Ü"},
        {"iexcl","¡"},{"iquest","¿"},{"laquo","«"},{"raquo","»"},
        {"mdash","—"},{"ndash","–"},{"hellip","…"},{"middot","·"},{"deg","°"},
        {"copy","©"},{"reg","®"},{"trade","™"},{"euro","€"},{"pound","£"},
        {"bull","•"},{"lsquo","‘"},{"rsquo","’"},{"ldquo","“"},{"rdquo","”"},
        {"times","×"},{"divide","÷"},{"plusmn","±"},{"micro","µ"},{"para","¶"},
        {"sect","§"},{"ordf","ª"},{"ordm","º"},{"ccedil","ç"},{"Ccedil","Ç"},
        {"agrave","à"},{"egrave","è"},{"igrave","ì"},{"ograve","ò"},{"ugrave","ù"},
        {"acirc","â"},{"ecirc","ê"},{"icirc","î"},{"ocirc","ô"},{"ucirc","û"},
    };
    std::string out;
    out.reserve(in.size());
    for (size_t i = 0; i < in.size(); ++i) {
        if (in[i] != '&') { out += in[i]; continue; }
        size_t semi = in.find(';', i + 1);
        if (semi == std::string::npos || semi - i > 12) { out += in[i]; continue; }
        std::string ent = in.substr(i + 1, semi - i - 1);
        if (ent.empty()) { out += in[i]; continue; }
        if (ent[0] == '#') {
            try {
                uint32_t cp;
                if (ent.size() > 1 && (ent[1] == 'x' || ent[1] == 'X'))
                    cp = (uint32_t)std::stoul(ent.substr(2), nullptr, 16);
                else
                    cp = (uint32_t)std::stoul(ent.substr(1), nullptr, 10);
                push_utf8(out, cp);
                i = semi;
                continue;
            } catch (...) { out += in[i]; continue; }
        }
        bool found = false;
        for (auto& [name, val] : named) {
            if (ent == name) { out += val; i = semi; found = true; break; }
        }
        if (!found) out += in[i]; // desconocida → se deja tal cual (honesto)
    }
    return out;
}

// --- charset ----------------------------------------------------------------
static std::string latin1_to_utf8(const std::string& in) {
    std::string out;
    out.reserve(in.size() * 2);
    for (unsigned char c : in) {
        if (c < 0x80) out += (char)c;
        else { out += (char)(0xC0 | (c >> 6)); out += (char)(0x80 | (c & 0x3F)); }
    }
    return out;
}

// --- CSS: @media REAL evaluado contra viewport (2026-08-11) ------------------
// Antes se borraban TODOS los bloques @media (strip ciego). Ahora se evalúan
// de verdad: solo se conservan los que matchean el viewport actual. Es la
// diferencia entre "simular que no existe responsive" y "soporte real honesto".
// Soporta: max-width/min-width/max-height/min-height (px), screen/all/print,
// and, comma (or), not. Otros features se asumen como no-match honesto.

static inline bool media_condition_matches(const std::string& cond, int vw, int vh) {
    std::string s = core::StringUtils::to_lower(core::StringUtils::trim(cond));
    if (s.empty()) return true;
    // split por coma (OR) respetando paréntesis
    std::vector<std::string> or_parts;
    {
        std::string cur; int depth=0;
        for (char c: s) {
            if (c=='(') depth++;
            else if (c==')') depth--;
            if (c==',' && depth==0) { or_parts.push_back(cur); cur.clear(); }
            else cur+=c;
        }
        or_parts.push_back(cur);
    }
    for (auto& part_raw: or_parts) {
        std::string p = core::StringUtils::trim(part_raw);
        if (p.empty()) continue;
        bool neg = false;
        if (p.rfind("not ",0)==0) { neg=true; p=core::StringUtils::trim(p.substr(4)); }
        if (p.rfind("only ",0)==0) p=core::StringUtils::trim(p.substr(5));

        // Split por " and " (case-insensitive, ya lower)
        std::vector<std::string> and_tokens;
        {
            std::string lower_p = p;
            size_t start=0;
            while (true) {
                size_t pos = lower_p.find(" and ", start);
                if (pos==std::string::npos) {
                    and_tokens.push_back(core::StringUtils::trim(p.substr(start)));
                    break;
                }
                and_tokens.push_back(core::StringUtils::trim(p.substr(start, pos-start)));
                start = pos+5;
            }
        }

        bool part_match = true;
        for (auto& tok_raw: and_tokens) {
            std::string tok = core::StringUtils::trim(tok_raw);
            std::string low = core::StringUtils::to_lower(tok);
            if (low=="screen" || low=="all" || low=="(all)" || low=="") continue;
            if (low=="print" || low=="speech") { part_match=false; break; }
            // Debe ser (feature: value)
            if (tok.size()>=2 && tok.front()=='(' && tok.back()==')') {
                std::string inner = core::StringUtils::trim(tok.substr(1,tok.size()-2));
                size_t colon = inner.find(':');
                if (colon==std::string::npos) { part_match=false; break; }
                std::string feat = core::StringUtils::trim(inner.substr(0,colon));
                std::string val = core::StringUtils::trim(inner.substr(colon+1));
                feat = core::StringUtils::to_lower(feat);
                val = core::StringUtils::to_lower(val);
                if (val.size()>2 && val.substr(val.size()-2)=="px") val=val.substr(0,val.size()-2);
                try {
                    if (feat=="orientation") {
                        bool is_portrait = vh >= vw;
                        if (val=="portrait" && !is_portrait) part_match=false;
                        if (val=="landscape" && is_portrait) part_match=false;
                    } else {
                        int num = std::stoi(val);
                        if (feat=="max-width") { if (!(vw <= num)) part_match=false; }
                        else if (feat=="min-width") { if (!(vw >= num)) part_match=false; }
                        else if (feat=="max-height") { if (!(vh <= num)) part_match=false; }
                        else if (feat=="min-height") { if (!(vh >= num)) part_match=false; }
                        else if (feat=="width") { if (!(vw == num)) part_match=false; }
                        else if (feat=="height") { if (!(vh == num)) part_match=false; }
                        else { part_match=false; }
                    }
                } catch (...) { part_match=false; }
                if (!part_match) break;
            } else {
                // token suelto desconocido => no match
                part_match=false; break;
            }
        }
        if (neg) part_match = !part_match;
        if (part_match) return true;
    }
    return false;
}

static std::string filter_media(const std::string& css, int vw, int vh) {
    std::string out;
    size_t i=0;
    while (i < css.size()) {
        if (css[i] != '@') { out+=css[i++]; continue; }
        // Detectar @media vs otros @
        if (core::StringUtils::to_lower(css.substr(i, 6)) == "@media") {
            size_t brace = css.find('{', i);
            if (brace==std::string::npos) { out+=css.substr(i); break; }
            std::string cond = css.substr(i+6, brace-(i+6));
            // Encontrar cierre balanceado del bloque @media
            int depth=0;
            size_t j=brace;
            for (; j<css.size(); ++j) {
                if (css[j]=='{') depth++;
                else if (css[j]=='}') { depth--; if (depth==0) { ++j; break; } }
            }
            std::string inner = css.substr(brace+1, (j>brace+1? j-brace-2 : 0));
            if (media_condition_matches(cond, vw, vh)) {
                // Mantener el contenido interior (sin el wrapper @media)
                // Recursivo: el interior puede tener más @media
                out += filter_media(inner, vw, vh);
                out += "\n";
            }
            i=j;
            continue;
        }
        // Otros @-rules: @import ...; @font-face {...} @keyframes {...} @supports
        // Los que son con ; se saltan, los de bloque se saltan entero (honesto: no soportados aún)
        size_t semi = css.find(';', i);
        size_t brace = css.find('{', i);
        if (brace==std::string::npos || (semi!=std::string::npos && semi < brace)) {
            i = (semi==std::string::npos) ? css.size() : semi+1;
            continue;
        }
        int depth=0; size_t j=brace;
        for (; j<css.size(); ++j) {
            if (css[j]=='{') depth++;
            else if (css[j]=='}') { depth--; if (depth==0){ ++j; break; } }
        }
        i=j;
        continue;
    }
    return out;
}

inline static std::string strip_at_rules(const std::string& css) {
    // Compat: sin viewport usa 1024x768 (fallback honesto para tests viejos)
    return filter_media(css, 1024, 768);
}
inline static std::string strip_at_rules(const std::string& css, int vw, int vh) {
    return filter_media(css, vw, vh);
}

// --- helpers de atributos ---------------------------------------------------
static std::string attr_value(const std::string& tag_src, const char* name) {
    // busca name= (insensible a mayúsculas) en la etiqueta
    std::string n = core::StringUtils::to_lower(tag_src);
    std::string key = core::StringUtils::to_lower(name);
    size_t p = n.find(key);
    while (p != std::string::npos) {
        bool boundary = (p == 0) || !(std::isalnum((unsigned char)n[p-1]) || n[p-1]=='-' || n[p-1]=='_');
        if (boundary) {
            size_t q = p + key.size();
            while (q < tag_src.size() && std::isspace((unsigned char)tag_src[q])) ++q;
            if (q < tag_src.size() && tag_src[q] == '=') {
                ++q;
                while (q < tag_src.size() && std::isspace((unsigned char)tag_src[q])) ++q;
                if (q < tag_src.size() && (tag_src[q] == '"' || tag_src[q] == '\'')) {
                    char quote = tag_src[q++];
                    size_t e = tag_src.find(quote, q);
                    return tag_src.substr(q, e == std::string::npos ? tag_src.size() - q : e - q);
                }
                size_t e = q;
                while (e < tag_src.size() && !std::isspace((unsigned char)tag_src[e]) && tag_src[e] != '>') ++e;
                return tag_src.substr(q, e - q);
            }
        }
        p = n.find(key, p + 1);
    }
    return "";
}

inline static bool attr_present(const std::string& tag_src, const char* name) {
    std::string n = core::StringUtils::to_lower(tag_src);
    return n.find(core::StringUtils::to_lower(name)) != std::string::npos;
}

// --- pipeline principal -----------------------------------------------------
static PreparedPage prepare(const std::string& raw, const std::string& page_url) {
    PreparedPage pp;
    std::string out;
    out.reserve(raw.size());

    size_t i = 0;
    while (i < raw.size()) {
        if (raw[i] != '<') { out += raw[i++]; continue; }

        // comentarios: los deja pasar el parser; skip manual por seguridad
        if (ieq_at(raw, i, "<!--")) {
            size_t e = raw.find("-->", i + 4);
            i = (e == std::string::npos) ? raw.size() : e + 3;
            continue;
        }

        // <style ...> ... </style>
        if (ieq_at(raw, i, "<style")) {
            size_t open_end = raw.find('>', i);
            if (open_end == std::string::npos) break;
            size_t close = find_close(raw, open_end + 1, "style");
            if (close == std::string::npos) { i = raw.size(); break; }
            pp.inline_css += "\n" + raw.substr(open_end + 1, close - open_end - 1);
            size_t close_end = raw.find('>', close);
            i = (close_end == std::string::npos) ? raw.size() : close_end + 1;
            continue;
        }

        // <script ...> ... </script>
        if (ieq_at(raw, i, "<script")) {
            size_t open_end = raw.find('>', i);
            if (open_end == std::string::npos) break;
            std::string open_tag = raw.substr(i, open_end - i + 1);
            size_t close = find_close(raw, open_end + 1, "script");
            size_t content_end = (close == std::string::npos) ? raw.size() : close;
            std::string src = attr_value(open_tag, "src");
            if (src.empty()) {
                std::string code = raw.substr(open_end + 1, content_end - open_end - 1);
                if (!core::StringUtils::trim(code).empty()) pp.inline_scripts.push_back(code);
            } else {
                // Script externo REAL: lo descarga BrowserShell como con CSS
                pp.external_scripts.push_back(net::Fetcher::resolve_url(page_url, src));
            }
            if (close == std::string::npos) { i = raw.size(); break; }
            size_t close_end = raw.find('>', close);
            i = (close_end == std::string::npos) ? raw.size() : close_end + 1;
            continue;
        }

        // <img> REAL (2026-08-09): ANTES se sustituía por un span de texto
        // "[imagen: alt]". Ya no: el navegador descarga el src, lo decodifica
        // con el decodificador PNG propio y lo pinta; si no se puede, marca
        // data-nuby-imgfail y la CSS UA dibuja un placeholder HONESTO.

        // <svg .....</svg> → placeholder
        if (ieq_at(raw, i, "<svg")) {
            size_t end = find_close(raw, i, "svg");
            if (end == std::string::npos) {
                size_t gt = raw.find('>', i);
                i = (gt == std::string::npos) ? raw.size() : gt + 1;
            } else {
                size_t gt = raw.find('>', end);
                out += "<span data-nuby-ph=\"svg\">[grafico vectorial]</span>";
                i = (gt == std::string::npos) ? raw.size() : gt + 1;
            }
            continue;
        }

        // <video>/<audio>/<iframe> → placeholder
        if (ieq_at(raw, i, "<video") || ieq_at(raw, i, "<audio") || ieq_at(raw, i, "<iframe")) {
            const char* tag = ieq_at(raw, i, "<video") ? "video" : (ieq_at(raw, i, "<audio") ? "audio" : "iframe");
            std::string label = std::string(tag) == "iframe" ? "marco externo omitido" : (std::string(tag) == "video" ? "video externo" : "audio externo");
            size_t open_end = raw.find('>', i);
            if (open_end == std::string::npos) break;
            std::string open_tag = raw.substr(i, open_end - i + 1);
            std::string src = attr_value(open_tag, "src");
            if (!src.empty()) { label += ": " + core::StringUtils::trim(src).substr(0, 60); }
            out += "<div data-nuby-ph=\"" + std::string(tag) + "\">[" + label + "]</div>";
            size_t close = find_close(raw, open_end + 1, tag);
            if (close == std::string::npos) { i = open_end + 1; continue; }
            size_t close_end = raw.find('>', close);
            i = (close_end == std::string::npos) ? raw.size() : close_end + 1;
            continue;
        }

        // <input> REAL (2026-08-09): ANTES aquí se REEMPLAZABA cada input por
        // un texto estático "[boton: X]" / "campo de texto ✎" — era una
        // simulación. Ya no: los inputs llegan íntegros al DOM y Nuby los
        // pinta, enfoca, edita y envía de verdad (ver browser_shell.hpp).

        // <base href="...">
        if (ieq_at(raw, i, "<base")) {
            size_t end = raw.find('>', i);
            if (end != std::string::npos) {
                std::string tag = raw.substr(i, end - i + 1);
                std::string href = attr_value(tag, "href");
                if (!href.empty()) pp.base_href = href;
                i = end + 1; continue;
            }
        }

        // <link rel=stylesheet href=...>
        if (ieq_at(raw, i, "<link")) {
            size_t end = raw.find('>', i);
            if (end != std::string::npos) {
                std::string tag = raw.substr(i, end - i + 1);
                std::string rel = core::StringUtils::to_lower(attr_value(tag, "rel"));
                std::string href = attr_value(tag, "href");
                if (rel.find("stylesheet") != std::string::npos && !href.empty()) {
                    pp.external_css.push_back(net::Fetcher::resolve_url(page_url, href));
                }
                i = end + 1; continue;
            }
        }

        // <select> REAL (2026-08-11): antes era placeholder [lista desplegable] — simulación.
        // Ahora pasa REAL al DOM: el motor lo pinta, permite elegir opción y lo envía en forms.
        // No se intercepta aquí, cae al flujo normal de etiquetas.

        // resto de etiquetas y texto: tal cual
        out += raw[i++];
    }

    pp.body_html = decode_entities(out);
    return pp;
}

} // namespace nuby::preprocess
