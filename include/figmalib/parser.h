#pragma once
// figmalib — Figma REST API JSON parser.
//
// Accepts the JSON returned by `GET /v1/files/:key?geometry=paths`
// (or an equivalent local export). The top-level object must contain a
// "document" node; a bare node tree (object with "id"/"type") also works.

#include <memory>
#include <string>

#include "document.h"

namespace figmalib {

// Parse Figma file JSON from a string. Throws std::runtime_error on malformed input.
std::unique_ptr<Document> parseDocument(const std::string& jsonText);

// Parse fig2json "canvas.json" output (the .fig-derived format). Handles
// type inference, component-instance hydration and shared-style resolution.
std::unique_ptr<Document> parseCanvasDocument(const std::string& jsonText);

// Convenience: read a file from disk and parse it (REST API JSON only).
std::unique_ptr<Document> loadDocumentFile(const std::string& path);

struct LoadedFile {
    std::unique_ptr<Document> document;
    std::string imageDirectory;  // images extracted from a .fig; empty otherwise
};

// Load any supported input:
//   *.fig          → converted via the fig2json CLI (see below), then parsed
//   canvas.json    → fig2json format (auto-detected)
//   *.json         → Figma REST API format
// fig2json discovery: $FIGMALIB_FIG2JSON, then PATH, then the compile-time
// default (FIGMALIB_FIG2JSON_DEFAULT). Conversion output is cached next to
// the .fig file in "<name>.fig.export/" and reused while up to date.
LoadedFile loadFigmaFile(const std::string& path);

// Serialize a document to Figma-REST-style JSON that parseDocument() reads
// back losslessly (figmalib's save format). Runtime overrides are not saved.
std::string writeDocumentJson(const Document& doc);
bool saveDocumentFile(const Document& doc, const std::string& path);

}  // namespace figmalib
