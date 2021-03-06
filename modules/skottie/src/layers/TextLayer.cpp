/*
 * Copyright 2018 Google Inc.
 *
 * Use of this source code is governed by a BSD-style license that can be
 * found in the LICENSE file.
 */

#include "modules/skottie/src/SkottiePriv.h"

#include "include/core/SkData.h"
#include "include/core/SkFontMgr.h"
#include "include/core/SkTypes.h"
#include "modules/skottie/src/SkottieJson.h"
#include "modules/skottie/src/text/TextAdapter.h"
#include "modules/skottie/src/text/TextAnimator.h"
#include "modules/skottie/src/text/TextValue.h"
#include "modules/sksg/include/SkSGDraw.h"
#include "modules/sksg/include/SkSGGroup.h"
#include "modules/sksg/include/SkSGPaint.h"
#include "modules/sksg/include/SkSGPath.h"
#include "modules/sksg/include/SkSGText.h"

#include <string.h>

namespace skottie {
namespace internal {

namespace {

SkFontStyle FontStyle(const AnimationBuilder* abuilder, const char* style) {
    static constexpr struct {
        const char*               fName;
        const SkFontStyle::Weight fWeight;
    } gWeightMap[] = {
        { "Regular"   , SkFontStyle::kNormal_Weight     },
        { "Medium"    , SkFontStyle::kMedium_Weight     },
        { "Bold"      , SkFontStyle::kBold_Weight       },
        { "Light"     , SkFontStyle::kLight_Weight      },
        { "Black"     , SkFontStyle::kBlack_Weight      },
        { "Thin"      , SkFontStyle::kThin_Weight       },
        { "Extra"     , SkFontStyle::kExtraBold_Weight  },
        { "ExtraBold" , SkFontStyle::kExtraBold_Weight  },
        { "ExtraLight", SkFontStyle::kExtraLight_Weight },
        { "ExtraBlack", SkFontStyle::kExtraBlack_Weight },
        { "SemiBold"  , SkFontStyle::kSemiBold_Weight   },
        { "Hairline"  , SkFontStyle::kThin_Weight       },
        { "Normal"    , SkFontStyle::kNormal_Weight     },
        { "Plain"     , SkFontStyle::kNormal_Weight     },
        { "Standard"  , SkFontStyle::kNormal_Weight     },
        { "Roman"     , SkFontStyle::kNormal_Weight     },
        { "Heavy"     , SkFontStyle::kBlack_Weight      },
        { "Demi"      , SkFontStyle::kSemiBold_Weight   },
        { "DemiBold"  , SkFontStyle::kSemiBold_Weight   },
        { "Ultra"     , SkFontStyle::kExtraBold_Weight  },
        { "UltraBold" , SkFontStyle::kExtraBold_Weight  },
        { "UltraBlack", SkFontStyle::kExtraBlack_Weight },
        { "UltraHeavy", SkFontStyle::kExtraBlack_Weight },
        { "UltraLight", SkFontStyle::kExtraLight_Weight },
    };

    SkFontStyle::Weight weight = SkFontStyle::kNormal_Weight;
    for (const auto& w : gWeightMap) {
        const auto name_len = strlen(w.fName);
        if (!strncmp(style, w.fName, name_len)) {
            weight = w.fWeight;
            style += name_len;
            break;
        }
    }

    static constexpr struct {
        const char*              fName;
        const SkFontStyle::Slant fSlant;
    } gSlantMap[] = {
        { "Italic" , SkFontStyle::kItalic_Slant  },
        { "Oblique", SkFontStyle::kOblique_Slant },
    };

    SkFontStyle::Slant slant = SkFontStyle::kUpright_Slant;
    if (*style != '\0') {
        for (const auto& s : gSlantMap) {
            if (!strcmp(style, s.fName)) {
                slant = s.fSlant;
                style += strlen(s.fName);
                break;
            }
        }
    }

    if (*style != '\0') {
        abuilder->log(Logger::Level::kWarning, nullptr, "Unknown font style: %s.", style);
    }

    return SkFontStyle(weight, SkFontStyle::kNormal_Width, slant);
}

bool parse_glyph_path(const skjson::ObjectValue* jdata,
                      const AnimationBuilder* abuilder,
                      SkPath* path) {
    // Glyph path encoding:
    //
    //   "data": {
    //       "shapes": [                         // follows the shape layer format
    //           {
    //               "ty": "gr",                 // group shape type
    //               "it": [                     // group items
    //                   {
    //                       "ty": "sh",         // actual shape
    //                       "ks": <path data>   // animatable path format, but always static
    //                   },
    //                   ...
    //               ]
    //           },
    //           ...
    //       ]
    //   }

    if (!jdata) {
        return false;
    }

    const skjson::ArrayValue* jshapes = (*jdata)["shapes"];
    if (!jshapes) {
        // Space/empty glyph.
        return true;
    }

    for (const skjson::ObjectValue* jgrp : *jshapes) {
        if (!jgrp) {
            return false;
        }

        const skjson::ArrayValue* jit = (*jgrp)["it"];
        if (!jit) {
            return false;
        }

        for (const skjson::ObjectValue* jshape : *jit) {
            if (!jshape) {
                return false;
            }

            // Glyph paths should never be animated.  But they are encoded as
            // animatable properties, so we use the appropriate helpers.
            AnimationBuilder::AutoScope ascope(abuilder);
            auto path_node = abuilder->attachPath((*jshape)["ks"]);
            auto animators = ascope.release();

            if (!path_node || !animators.empty()) {
                return false;
            }

            // Successfully parsed a static path.  Whew.
            path->addPath(path_node->getPath());
        }
    }

    return true;
}

} // namespace

bool AnimationBuilder::FontInfo::matches(const char family[], const char style[]) const {
    return 0 == strcmp(fFamily.c_str(), family)
        && 0 == strcmp(fStyle.c_str(), style);
}

#ifdef SK_NO_FONTS
void AnimationBuilder::parseFonts(const skjson::ObjectValue* jfonts,
                                  const skjson::ArrayValue* jchars) {}

sk_sp<sksg::RenderNode> AnimationBuilder::attachTextLayer(const skjson::ObjectValue& jlayer,
                                                          LayerInfo*) const {
    return nullptr;
}
#else
void AnimationBuilder::parseFonts(const skjson::ObjectValue* jfonts,
                                  const skjson::ArrayValue* jchars) {
    // Optional array of font entries, referenced (by name) from text layer document nodes. E.g.
    // "fonts": {
    //        "list": [
    //            {
    //                "ascent": 75,
    //                "fClass": "",
    //                "fFamily": "Roboto",
    //                "fName": "Roboto-Regular",
    //                "fPath": "https://fonts.googleapis.com/css?family=Roboto",
    //                "fPath": "",
    //                "fStyle": "Regular",
    //                "fWeight": "",
    //                "origin": 1
    //            }
    //        ]
    //    },
    const skjson::ArrayValue* jlist = jfonts
            ? static_cast<const skjson::ArrayValue*>((*jfonts)["list"])
            : nullptr;
    if (!jlist) {
        return;
    }

    // First pass: collect font info.
    for (const skjson::ObjectValue* jfont : *jlist) {
        if (!jfont) {
            continue;
        }

        const skjson::StringValue* jname   = (*jfont)["fName"];
        const skjson::StringValue* jfamily = (*jfont)["fFamily"];
        const skjson::StringValue* jstyle  = (*jfont)["fStyle"];
        const skjson::StringValue* jpath   = (*jfont)["fPath"];

        if (!jname   || !jname->size() ||
            !jfamily || !jfamily->size() ||
            !jstyle  || !jstyle->size()) {
            this->log(Logger::Level::kError, jfont, "Invalid font.");
            continue;
        }

        fFonts.set(SkString(jname->begin(), jname->size()),
                  {
                      SkString(jfamily->begin(), jfamily->size()),
                      SkString( jstyle->begin(),  jstyle->size()),
                      jpath ? SkString(  jpath->begin(),   jpath->size()) : SkString(),
                      ParseDefault((*jfont)["ascent"] , 0.0f),
                      nullptr, // placeholder
                      SkCustomTypefaceBuilder()
                  });
    }

    // Optional pass.
    if (jchars && (fFlags & Animation::Builder::kPreferEmbeddedFonts) &&
        this->resolveEmbeddedTypefaces(*jchars)) {
        return;
    }

    // Native typeface resolution.
    if (this->resolveNativeTypefaces()) {
        return;
    }

    // Embedded typeface fallback.
    if (jchars && !(fFlags & Animation::Builder::kPreferEmbeddedFonts) &&
        this->resolveEmbeddedTypefaces(*jchars)) {
    }
}

bool AnimationBuilder::resolveNativeTypefaces() {
    bool has_unresolved = false;

    fFonts.foreach([&](const SkString& name, FontInfo* finfo) {
        SkASSERT(finfo);

        if (finfo->fTypeface) {
            // Already resolved from glyph paths.
            return;
        }

        const auto& fmgr = fLazyFontMgr.get();

        // Typeface fallback order:
        //   1) externally-loaded font (provided by the embedder)
        //   2) system font (family/style)
        //   3) system default

        finfo->fTypeface =
            fmgr->makeFromData(fResourceProvider->loadFont(name.c_str(), finfo->fPath.c_str()));

        if (!finfo->fTypeface) {
            finfo->fTypeface.reset(fmgr->matchFamilyStyle(finfo->fFamily.c_str(),
                                                          FontStyle(this, finfo->fStyle.c_str())));

            if (!finfo->fTypeface) {
                this->log(Logger::Level::kError, nullptr, "Could not create typeface for %s|%s.",
                          finfo->fFamily.c_str(), finfo->fStyle.c_str());
                // Last resort.
                finfo->fTypeface = fmgr->legacyMakeTypeface(nullptr,
                                                            FontStyle(this, finfo->fStyle.c_str()));

                has_unresolved |= !finfo->fTypeface;
            }
        }
    });

    return !has_unresolved;
}

bool AnimationBuilder::resolveEmbeddedTypefaces(const skjson::ArrayValue& jchars) {
    // Optional array of glyphs, to be associated with one of the declared fonts. E.g.
    // "chars": [
    //     {
    //         "ch": "t",
    //         "data": {
    //             "shapes": [...]        // shape-layer-like geometry
    //         },
    //         "fFamily": "Roboto",       // part of the font key
    //         "size": 50,                // apparently ignored
    //         "style": "Regular",        // part of the font key
    //         "w": 32.67                 // width/advance (1/100 units)
    //    }
    // ]
    FontInfo* current_font = nullptr;

    for (const skjson::ObjectValue* jchar : jchars) {
        if (!jchar) {
            continue;
        }

        const skjson::StringValue* jch = (*jchar)["ch"];
        if (!jch) {
            continue;
        }

        const skjson::StringValue* jfamily = (*jchar)["fFamily"];
        const skjson::StringValue* jstyle  = (*jchar)["style"]; // "style", not "fStyle"...

        const auto* ch_ptr = jch->begin();
        const auto  ch_len = jch->size();

        if (!jfamily || !jstyle || (SkUTF::CountUTF8(ch_ptr, ch_len) != 1)) {
            this->log(Logger::Level::kError, jchar, "Invalid glyph.");
            continue;
        }

        const auto uni = SkUTF::NextUTF8(&ch_ptr, ch_ptr + ch_len);
        SkASSERT(uni != -1);
        if (!SkTFitsIn<SkGlyphID>(uni)) {
            // Custom font keys are SkGlyphIDs.  We could implement a remapping scheme if needed,
            // but for now direct mapping seems to work well enough.
            this->log(Logger::Level::kError, jchar, "Unsupported glyph ID.");
            continue;
        }
        const auto glyph_id = SkTo<SkGlyphID>(uni);

        const auto* family = jfamily->begin();
        const auto* style  = jstyle->begin();

        // Locate (and cache) the font info. Unlike text nodes, glyphs reference the font by
        // (family, style) -- not by name :(  For now this performs a linear search over *all*
        // fonts: generally there are few of them, and glyph definitions are font-clustered.
        // If problematic, we can refactor as a two-level hashmap.
        if (!current_font || !current_font->matches(family, style)) {
            current_font = nullptr;
            fFonts.foreach([&](const SkString& name, FontInfo* finfo) {
                if (finfo->matches(family, style)) {
                    current_font = finfo;
                    // TODO: would be nice to break early here...
                }
            });
            if (!current_font) {
                this->log(Logger::Level::kError, nullptr,
                          "Font not found for codepoint (%d, %s, %s).", uni, family, style);
                continue;
            }
        }

        SkPath path;
        if (!parse_glyph_path((*jchar)["data"], this, &path)) {
            continue;
        }

        const auto advance = ParseDefault((*jchar)["w"], 0.0f);

        // Interestingly, glyph paths are defined in a percentage-based space,
        // regardless of declared glyph size...
        static constexpr float kPtScale = 0.01f;

        // Normalize the path and advance for 1pt.
        path.transform(SkMatrix::Scale(kPtScale, kPtScale));

        current_font->fCustomBuilder.setGlyph(glyph_id, advance * kPtScale, path);
    }

    // Final pass to commit custom typefaces.
    auto has_unresolved = false;
    fFonts.foreach([&has_unresolved](const SkString&, FontInfo* finfo) {
        if (finfo->fTypeface) {
            return; // already resolved
        }

        finfo->fTypeface = finfo->fCustomBuilder.detach();

        has_unresolved |= !finfo->fTypeface;
    });

    return !has_unresolved;
}

sk_sp<sksg::RenderNode> AnimationBuilder::attachTextLayer(const skjson::ObjectValue& jlayer,
                                                          LayerInfo*) const {
    return this->attachDiscardableAdapter<TextAdapter>(jlayer,
                                                       this,
                                                       fLazyFontMgr.getMaybeNull(),
                                                       fLogger);
}
#endif

const AnimationBuilder::FontInfo* AnimationBuilder::findFont(const SkString& font_name) const {
    return fFonts.find(font_name);
}

} // namespace internal
} // namespace skottie
