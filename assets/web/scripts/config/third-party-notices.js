/**
 * Third-party notices — the single source of truth for everything the web UI
 * ships that Aqualink Automate did not write, and for the licence line shown on
 * the About page. The About view (scripts/views/about-view.js) renders its
 * "Licences" section straight from this object, so a notice is written once.
 *
 * Loaded as a classic (non-module) script like the other config, exposing
 * `window.AquaNotices`. It is plain data: no user-visible text here goes through
 * the i18n catalogue because it is legal text and proper names, which must
 * appear exactly as the licensors wrote them.
 *
 * Keeping it honest:
 *   - `files` lists where each component ships, relative to assets/web. A `*`
 *     matches within one path segment. e2e/about-licences.spec.ts fails if a
 *     file under vendor/ is not claimed by an entry, so vendoring a new library
 *     or font means adding it here in the same change.
 *   - Copyright lines are copied from the component itself: a font's `name`
 *     table (ID 0), a script's own header comment, or the upstream LICENSE file
 *     at the vendored version. Do not paraphrase them.
 *   - `version` is what the vendored file says it is, when it says.
 *
 * To change the application's own copyright line, edit `app.copyright` below.
 */
(function (global) {
    'use strict';

    const LICENCES = {
        'OFL-1.1': {
            name: 'SIL Open Font License, Version 1.1',
            url: 'https://openfontlicense.org',
            text: `SIL OPEN FONT LICENSE Version 1.1 - 26 February 2007
-----------------------------------------------------------

PREAMBLE
The goals of the Open Font License (OFL) are to stimulate worldwide
development of collaborative font projects, to support the font creation
efforts of academic and linguistic communities, and to provide a free and
open framework in which fonts may be shared and improved in partnership
with others.

The OFL allows the licensed fonts to be used, studied, modified and
redistributed freely as long as they are not sold by themselves. The
fonts, including any derivative works, can be bundled, embedded,
redistributed and/or sold with any software provided that any reserved
names are not used by derivative works. The fonts and derivatives,
however, cannot be released under any other type of license. The
requirement for fonts to remain under this license does not apply
to any document created using the fonts or their derivatives.

DEFINITIONS
"Font Software" refers to the set of files released by the Copyright
Holder(s) under this license and clearly marked as such. This may
include source files, build scripts and documentation.

"Reserved Font Name" refers to any names specified as such after the
copyright statement(s).

"Original Version" refers to the collection of Font Software components as
distributed by the Copyright Holder(s).

"Modified Version" refers to any derivative made by adding to, deleting,
or substituting -- in part or in whole -- any of the components of the
Original Version, by changing formats or by porting the Font Software to a
new environment.

"Author" refers to any designer, engineer, programmer, technical
writer or other person who contributed to the Font Software.

PERMISSION & CONDITIONS
Permission is hereby granted, free of charge, to any person obtaining
a copy of the Font Software, to use, study, copy, merge, embed, modify,
redistribute, and sell modified and unmodified copies of the Font
Software, subject to the following conditions:

1) Neither the Font Software nor any of its individual components,
in Original or Modified Versions, may be sold by itself.

2) Original or Modified Versions of the Font Software may be bundled,
redistributed and/or sold with any software, provided that each copy
contains the above copyright notice and this license. These can be
included either as stand-alone text files, human-readable headers or
in the appropriate machine-readable metadata fields within text or
binary files as long as those fields can be easily viewed by the user.

3) No Modified Version of the Font Software may use the Reserved Font
Name(s) unless explicit written permission is granted by the corresponding
Copyright Holder. This restriction only applies to the primary font name as
presented to the users.

4) The name(s) of the Copyright Holder(s) or the Author(s) of the Font
Software shall not be used to promote, endorse or advertise any
Modified Version, except to acknowledge the contribution(s) of the
Copyright Holder(s) and the Author(s) or with their explicit written
permission.

5) The Font Software, modified or unmodified, in part or in whole,
must be distributed entirely under this license, and must not be
distributed under any other license. The requirement for fonts to
remain under this license does not apply to any document created
using the Font Software.

TERMINATION
This license becomes null and void if any of the above conditions are
not met.

DISCLAIMER
THE FONT SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO ANY WARRANTIES OF
MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT
OF COPYRIGHT, PATENT, TRADEMARK, OR OTHER RIGHT. IN NO EVENT SHALL THE
COPYRIGHT HOLDER BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY,
INCLUDING ANY GENERAL, SPECIAL, INDIRECT, INCIDENTAL, OR CONSEQUENTIAL
DAMAGES, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
FROM, OUT OF THE USE OR INABILITY TO USE THE FONT SOFTWARE OR FROM
OTHER DEALINGS IN THE FONT SOFTWARE.`,
        },
        'MIT': {
            name: 'MIT License',
            url: 'https://opensource.org/license/mit',
            text: `Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.`,
        },
    };

    // kind: 'font' | 'script' | 'icon' — the About page groups by it.
    const COMPONENTS = [
        // ---- Fonts (vendor/fonts/, declared in vendor/fonts/fonts.css) -------
        {
            kind: 'font',
            name: 'Bricolage Grotesque',
            version: '1.001',
            licence: 'OFL-1.1',
            copyright: ['Copyright 2022 The Bricolage Grotesque Project Authors (https://github.com/ateliertriay/bricolage)'],
            url: 'https://github.com/ateliertriay/bricolage',
            files: ['vendor/fonts/bricolage-*.woff2'],
        },
        {
            kind: 'font',
            name: 'Hanken Grotesk',
            version: '3.013',
            licence: 'OFL-1.1',
            copyright: ['Copyright 2021 The Hanken Grotesk Project Authors (https://github.com/marcologous/hanken-grotesk)'],
            url: 'https://github.com/marcologous/hanken-grotesk',
            files: ['vendor/fonts/hanken-*.woff2'],
        },
        {
            kind: 'font',
            name: 'Noto Sans Arabic',
            version: '2.012',
            licence: 'OFL-1.1',
            copyright: ['Copyright 2022 The Noto Project Authors (https://github.com/notofonts/arabic)'],
            url: 'https://github.com/notofonts/arabic',
            files: ['vendor/fonts/noto-arabic.woff2'],
        },
        {
            kind: 'font',
            name: 'Noto Sans Hebrew',
            version: '3.001',
            licence: 'OFL-1.1',
            copyright: ['Copyright 2024 The Noto Project Authors (https://github.com/notofonts/hebrew)'],
            url: 'https://github.com/notofonts/hebrew',
            files: ['vendor/fonts/noto-hebrew.woff2'],
        },

        // ---- Scripts (vendor/*.js) -------------------------------------------
        {
            kind: 'script',
            name: 'Alpine.js',
            version: '3.14.9',
            licence: 'MIT',
            copyright: ['Copyright © 2019-2021 Caleb Porzio and contributors'],
            url: 'https://github.com/alpinejs/alpine',
            files: ['vendor/alpine.min.js'],
        },
        {
            kind: 'script',
            name: 'Chart.js',
            version: '4.4.9',
            licence: 'MIT',
            copyright: ['Copyright (c) 2025 Chart.js Contributors'],
            url: 'https://www.chartjs.org',
            files: ['vendor/chart.umd.min.js'],
        },
        {
            kind: 'script',
            name: 'chartjs-adapter-date-fns',
            version: '3.0.0',
            licence: 'MIT',
            copyright: ['Copyright (c) 2022 chartjs-adapter-date-fns Contributors'],
            url: 'https://github.com/chartjs/chartjs-adapter-date-fns',
            files: ['vendor/chartjs-adapter-date-fns.min.js'],
        },
        {
            // Bundled inside the adapter file above (its jsDelivr "bundle" build).
            kind: 'script',
            name: 'date-fns',
            version: '2.19',
            licence: 'MIT',
            copyright: ['Copyright (C) 2020 Sasha Koss and Lesha Koss'],
            url: 'https://date-fns.org',
            files: ['vendor/chartjs-adapter-date-fns.min.js'],
        },
        {
            kind: 'script',
            name: 'QRCode.js',
            version: '1.0.0',
            licence: 'MIT',
            copyright: [
                'Copyright (c) 2012 davidshimjs',
                'Includes QRCode for JavaScript, Copyright (c) 2009 Kazuhiko Arase (http://www.d-project.com/)',
            ],
            notes: ['The word "QR Code" is a registered trademark of DENSO WAVE INCORPORATED.'],
            url: 'https://github.com/davidshimjs/qrcodejs',
            files: ['vendor/qrcode.min.js'],
        },

        // ---- Icons (inline in index.html) ------------------------------------
        {
            // The equipment sprite and the navigation icons are Feather paths,
            // some re-encoded or adjusted. Feather ships no version in the paths.
            kind: 'icon',
            name: 'Feather',
            licence: 'MIT',
            copyright: ['Copyright (c) 2013-2023 Cole Bemis'],
            url: 'https://feathericons.com',
            files: ['index.html'],
        },
    ];

    global.AquaNotices = Object.freeze({
        app: Object.freeze({
            licence: 'GNU General Public License v3.0',
            licenceFile: 'LICENSE.txt',
            copyright: 'Copyright © 2026 Iain Chesworth',
        }),
        licences: Object.freeze(LICENCES),
        components: Object.freeze(COMPONENTS),
    });
})(window);
