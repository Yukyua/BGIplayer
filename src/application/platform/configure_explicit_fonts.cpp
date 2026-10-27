namespace bgiplayer {
namespace {
std::string encode_explicit_face(std::string_view name, TextEncoding encoding) {
    std::string output;
    for (const uint32_t c : decode_text_code_points(name, TextEncoding::Utf8)) {
        output += encode_code_point(c, encoding);
    }
    return output;
}
std::string utf8_explicit_face(std::u16string_view name) {
    std::string output;
    for (std::size_t index = 0; index < name.size(); ++index) {
        uint32_t c = name[index];
        if (c >= 0xD800U && c <= 0xDBFFU && index + 1U < name.size() &&
            name[index + 1U] >= 0xDC00 && name[index + 1U] <= 0xDFFF) {
            c = ((c - 0xD800U) << 10U) + name[++index] - 0xDC00U + 0x10000U;
        }
        output += encode_code_point(c, TextEncoding::Utf8);
    }
    return output;
}
void configure_explicit_fonts(VM& vm, const std::map<std::string, std::filesystem::path>& files, const std::filesystem::path& default_font_file) {
    std::optional<HostFontFileFace> preferred;
    if (!default_font_file.empty()) {
        FreeTypeGlyphConfiguration config{default_font_file, 0, 24, 100, 0, 0, 0, 24, 1};
        if (!make_freetype_glyph_provider(config)) throw Error("cannot open default dialogue font");
        const auto metadata = inspect_font_file_faces(default_font_file);
        preferred = HostFontFileFace{metadata.empty() ? "Host default" : metadata.front().family,
            default_font_file, 0, metadata.empty() ? 0U : metadata.front().charset};
    }
    std::map<std::string, HostFontFileFace> mappings;
    const auto roots = host_font_search_roots();
    const auto available_fonts = discover_host_font_faces(roots);
    for (const auto& face : available_fonts) mappings.try_emplace(face.family, face);
    const auto originals = mappings;
    for (const auto& replacement : host_font_replacements()) {
        if (const auto family = originals.find(replacement.family); family != originals.end()) {
            mappings[replacement.requested] = family->second;
            if (debug_trace(DebugTrace::Fonts)) {
                std::cout << "FONT_HOST_REPLACEMENT requested=" << replacement.requested
                          << " family=" << replacement.family << " source=" << replacement.source << '\n';
            }
        }
    }
    for (const auto& [face, file] : files) {
        FreeTypeGlyphConfiguration config{file, 0, 24, 100, 0, 0, 0, 24, 1};
        if (!make_freetype_glyph_provider(config)) throw Error("cannot open --font file: " + file.string());
        const auto metadata = inspect_font_file_faces(file);
        const uint32_t charset = metadata.empty() ? 0U : metadata.front().charset;
        mappings[face] = {metadata.empty() ? face : metadata.front().family, file, 0, charset};
    }
    std::map<std::string, uint32_t> mapped_charsets;
    for (const auto& [face, mapping] : mappings) mapped_charsets[face] = mapping.charset;
    const auto default_cjk = select_default_cjk_font(available_fonts, "gothic");
    vm.set_font_enumerator([&vm, mapped_charsets, default_cjk, files]() {
        auto faces = enumerate_platform_fonts();
        const TextEncoding encoding = vm.text_encoding_mode() == 1 ? TextEncoding::Utf8 : TextEncoding::ShiftJis;
        for (const auto& [face, charset] : mapped_charsets) {
            std::string encoded;
            try { encoded = encode_explicit_face(face, encoding); }
            catch (const std::exception&) { continue; }
            std::erase_if(faces, [&](const VM::HostFontFace& old) { return old.name == encoded; });
            faces.push_back({encoded, charset});
        }
        if (files.empty() && default_cjk) {
            // Host enumeration order is platform-owned. Keep private UI faces
            // out of the default list and put a verified CJK face first.
            std::erase_if(faces, [](const auto& f){return !f.name.empty() && f.name.front()=='.';});
            try {
                const auto preferred=encode_explicit_face(default_cjk->family,encoding);
                std::erase_if(faces,[&](const auto& f){return f.name==preferred;});
                faces.insert(faces.begin(),{preferred,default_cjk->charset});
            } catch(const std::exception&) {}
        }
        return faces;
    });
    using Key = std::tuple<std::string, uint32_t, uint32_t, uint32_t>;
    auto cache = std::make_shared<std::map<Key, GlyphOutlineProvider>>();
    auto missing = std::make_shared<std::set<std::string>>();
    vm.set_window_glyph_provider_factory([mappings, available_fonts, cache, missing, files, preferred](const VM::WindowGlyphRequest& request)
        -> std::optional<GlyphOutlineProvider> {
        const std::string face = utf8_explicit_face(request.font.face);
        const auto mapping = mappings.find(face);
        std::optional<HostFontFileFace> fallback;
        // Explicit per-face CLI mappings retain precedence over the launcher default.
        const HostFontFileFace* selected = preferred && !files.contains(face) ? &*preferred :
            mapping == mappings.end() ? nullptr : &mapping->second;
        if (!selected) {
            fallback = select_default_cjk_font(available_fonts, face);
            if (fallback) {
                selected = &*fallback;
                if (debug_trace(DebugTrace::Fonts))
                    std::cout << "FONT_SYSTEM_FALLBACK requested=" << face << " family=" << selected->family
                          << " path=" << selected->file << " index=" << selected->face_index << '\n';
            } else {
                if (missing->insert(face).second)
                    std::cerr << "FONT_UNRESOLVED requested=" << face << " charset=" << static_cast<unsigned>(request.font.charset)
                              << " (use --font FACE=FILE or BGIPLAYER_FONT_DIR)\n";
                return std::nullopt;
            }
        }
        const Key key{face, request.logical_height, request.width_percent, request.bold};
        const auto found = cache->find(key);
        if (found != cache->end()) return found->second;
        FreeTypeGlyphConfiguration config{selected->file, selected->face_index,
            static_cast<int32_t>(request.logical_height), static_cast<int32_t>(request.width_percent),
            0, 0, (request.logical_height * request.width_percent) / 100U, request.logical_height, 1};
        auto provider = make_freetype_glyph_provider(config);
        if (provider) {
            const auto underlying = *provider;
            const auto selected_file = selected->file;
            const auto selected_family = selected->family;
            auto missing_glyphs = std::make_shared<std::set<uint32_t>>();
            std::optional<GlyphOutlineProvider> fallback_provider;
            std::optional<HostFontFileFace> fallback_face;
            if(!files.contains(face)) {
                fallback_face=select_default_cjk_font(available_fonts,face);
                if(fallback_face && (fallback_face->file!=selected_file || fallback_face->face_index!=selected->face_index)) {
                    auto fallback_config=config;fallback_config.font_file=fallback_face->file;
                    fallback_config.face_index=fallback_face->face_index;
                    fallback_provider=make_freetype_glyph_provider(fallback_config);
                }
            }
            provider = GlyphOutlineProvider{[underlying, face, selected_file, selected_family, missing_glyphs, fallback_provider, fallback_face](const GlyphFontRequest& font, uint32_t codepoint) {
                auto glyph = underlying(font, codepoint);
                if(!glyph && fallback_provider) {
                    glyph=(*fallback_provider)(font,codepoint);
                    if(glyph && debug_trace(DebugTrace::Fonts) &&
                       missing_glyphs->insert(codepoint).second)
                        std::cout<<"FONT_GLYPH_SYSTEM_FALLBACK requested="<<face<<" codepoint=U+"<<std::hex<<codepoint<<std::dec
                                 <<" family="<<fallback_face->family<<" path="<<fallback_face->file<<" index="<<fallback_face->face_index<<'\n';
                }
                if (!glyph && missing_glyphs->insert(codepoint).second)
                    std::cerr << "FONT_GLYPH_MISSING requested=" << face << " family=" << selected_family
                              << " path=" << selected_file << " codepoint=U+" << std::hex << codepoint << std::dec << '\n';
                return glyph;
            }};
            cache->emplace(key, *provider);
        } else {
            std::cerr << "FONT_UNAVAILABLE requested=" << face
                      << " path=" << selected->file
                      << " index=" << selected->face_index << '\n';
        }
        return provider;
    });
}
} // namespace
} // namespace bgiplayer
