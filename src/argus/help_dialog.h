// Argus — Query syntax help dialog (bound to F1).
#pragma once

#include <QDialog>
#include <QDialogButtonBox>
#include <QLabel>
#include <QScrollArea>
#include <QVBoxLayout>

class HelpDialog : public QDialog {
    Q_OBJECT
public:
    HelpDialog(QWidget* parent = nullptr) : QDialog(parent) {
        setWindowTitle("Argus — query syntax");
        resize(720, 620);

        auto* root = new QVBoxLayout(this);
        root->setContentsMargins(16, 14, 16, 12);

        auto* label = new QLabel();
        label->setTextFormat(Qt::RichText);
        label->setWordWrap(true);
        label->setOpenExternalLinks(true);
        label->setText(html());

        auto* scroll = new QScrollArea();
        scroll->setWidgetResizable(true);
        scroll->setWidget(label);
        root->addWidget(scroll, 1);

        auto* bb = new QDialogButtonBox(QDialogButtonBox::Close);
        connect(bb, &QDialogButtonBox::rejected, this, &QDialog::reject);
        root->addWidget(bb);
    }

private:
    static QString html() {
        return QStringLiteral(R"(
<h2>Query syntax</h2>
<p>Whitespace-separated tokens are all <b>AND</b>-combined. Prefix
<code>!</code> to negate a predicate.</p>

<h3>Name search</h3>
<ul>
  <li><code>report</code> — matches names containing <i>report</i></li>
  <li><code>readme.md</code> — matches any name containing <i>readme.md</i></li>
  <li>Combine: <code>report notes</code> — must contain both <i>report</i> and <i>notes</i></li>
</ul>

<h3>File type</h3>
<ul>
  <li><code>ext:pdf</code> — file extension</li>
  <li><code>type:image</code> — one of the built-in categories:
    <ul>
      <li><code>image</code> — jpg, png, gif, bmp, webp, svg, tif, heic…</li>
      <li><code>video</code> — mp4, mkv, avi, mov, wmv, webm…</li>
      <li><code>audio</code> — mp3, wav, flac, m4a, ogg…</li>
      <li><code>document</code> — pdf, doc, docx, xls, txt, md…</li>
      <li><code>archive</code> — zip, rar, 7z, tar, gz…</li>
      <li><code>code</code> — cpp, py, js, ts, go, rs, java…</li>
      <li><code>exe</code> — exe, msi, dll, sys</li>
    </ul>
  </li>
</ul>

<h3>Location</h3>
<ul>
  <li><code>path:downloads</code> — full path contains "downloads"</li>
  <li><code>path:\Users\andre\</code> — literal path fragment</li>
</ul>

<h3>Size</h3>
<ul>
  <li><code>size:&gt;100MB</code> — larger than 100 megabytes</li>
  <li><code>size:&lt;1GB</code> — smaller than 1 gigabyte</li>
  <li><code>size:&gt;=500K</code> — at least 500 kilobytes</li>
  <li>Suffixes: K, M, G, T (binary — 1K = 1024)</li>
</ul>

<h3>Time</h3>
<ul>
  <li><code>modified:&lt;7d</code> — modified within the last 7 days</li>
  <li><code>modified:&lt;24h</code> — within 24 hours</li>
  <li><code>modified:&lt;30m</code> — within 30 minutes</li>
  <li>Units: s, m, h, d, w, y</li>
</ul>

<h3>Negation</h3>
<ul>
  <li><code>!temp</code> — name must NOT contain "temp"</li>
  <li><code>!ext:log</code> — must NOT have extension .log</li>
  <li><code>!path:node_modules</code> — anywhere except node_modules trees</li>
</ul>

<h3>Real-world examples</h3>
<pre style="background: palette(base); padding: 8px; border-radius: 4px;">
report ext:pdf
type:video size:&gt;1GB
backup ext:zip modified:&lt;30d
type:image path:downloads !thumb
config ext:json size:&gt;1KB size:&lt;100KB
</pre>

<h3>Search modes (top-right dropdown)</h3>
<ul>
  <li><b>Text</b> — substring, fast, default</li>
  <li><b>Wildcard</b> — <code>*.mp4</code>, <code>read*.md</code></li>
  <li><b>Regex</b> — ECMAScript, case-insensitive</li>
  <li><b>Fuzzy</b> — score-based, matches subsequences (like fzf)</li>
</ul>

<h3>Shortcuts</h3>
<table cellpadding="4">
  <tr><td><code>Ctrl+F</code></td><td>focus &amp; select search</td></tr>
  <tr><td><code>F5</code></td><td>rescan current drive(s)</td></tr>
  <tr><td><code>Esc</code></td><td>clear search</td></tr>
  <tr><td><code>Ctrl+C</code></td><td>copy path(s)</td></tr>
  <tr><td><code>Ctrl+Enter</code></td><td>open containing folder</td></tr>
  <tr><td><code>Alt+Enter</code></td><td>Windows Properties dialog</td></tr>
  <tr><td><code>Delete</code></td><td>move to Recycle Bin</td></tr>
  <tr><td><code>F1</code></td><td>this help</td></tr>
</table>
)");
    }
};
