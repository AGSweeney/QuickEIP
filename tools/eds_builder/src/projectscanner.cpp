/* MIT License. Copyright (c) 2026 Adam G. Sweeney <agsweeney@gmail.com> */
#include "projectscanner.h"

#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QRegularExpression>

#include <algorithm>

namespace {

struct Symbol {
    QString expr;
    bool fromLibraryConfig = false;     /* a default in eip_config.h: anything else wins */
};

typedef QHash<QString, Symbol> SymbolTable;

/* Replace comments with spaces. Newlines and string literals stay, so offsets
 * and line numbers still match the file. */
QString stripComments(const QString &in)
{
    QString out = in;
    const int n = in.size();
    int i = 0;
    while (i < n) {
        const QChar ch = in[i];
        if (ch == QLatin1Char('"') || ch == QLatin1Char('\'')) {
            const QChar quote = ch;
            i++;
            while (i < n && in[i] != quote && in[i] != QLatin1Char('\n')) {
                i += (in[i] == QLatin1Char('\\') && i + 1 < n) ? 2 : 1;
            }
            i++;
        } else if (ch == QLatin1Char('/') && i + 1 < n && in[i + 1] == QLatin1Char('/')) {
            while (i < n && in[i] != QLatin1Char('\n')) {
                out[i++] = QLatin1Char(' ');
            }
        } else if (ch == QLatin1Char('/') && i + 1 < n && in[i + 1] == QLatin1Char('*')) {
            while (i < n && !(in[i] == QLatin1Char('*') && i + 1 < n && in[i + 1] == QLatin1Char('/'))) {
                if (in[i] != QLatin1Char('\n')) {
                    out[i] = QLatin1Char(' ');
                }
                i++;
            }
            if (i < n) {
                out[i] = QLatin1Char(' ');
                out[i + 1] = QLatin1Char(' ');
                i += 2;
            }
        } else {
            i++;
        }
    }
    return out;
}

int lineOf(const QString &text, int offset)
{
    return 1 + int(text.left(offset).count(QLatin1Char('\n')));
}

/* The offset of the bracket that closes the one at open, or -1. */
int matchBracket(const QString &s, int open)
{
    const QChar a = s[open];
    const QChar b = (a == QLatin1Char('{')) ? QLatin1Char('}') : (a == QLatin1Char('(')) ? QLatin1Char(')') : QLatin1Char(']');
    int depth = 0;
    for (int i = open; i < s.size(); i++) {
        const QChar ch = s[i];
        if (ch == QLatin1Char('"') || ch == QLatin1Char('\'')) {
            const QChar quote = ch;
            i++;
            while (i < s.size() && s[i] != quote) {
                i += (s[i] == QLatin1Char('\\')) ? 2 : 1;
            }
        } else if (ch == a) {
            depth++;
        } else if (ch == b) {
            if (--depth == 0) {
                return i;
            }
        }
    }
    return -1;
}

/* Split at commas that are not inside brackets or a string. */
QStringList splitTopLevel(const QString &s)
{
    QStringList parts;
    int depth = 0;
    int start = 0;
    for (int i = 0; i < s.size(); i++) {
        const QChar ch = s[i];
        if (ch == QLatin1Char('"') || ch == QLatin1Char('\'')) {
            const QChar quote = ch;
            i++;
            while (i < s.size() && s[i] != quote) {
                i += (s[i] == QLatin1Char('\\')) ? 2 : 1;
            }
        } else if (ch == QLatin1Char('(') || ch == QLatin1Char('{') || ch == QLatin1Char('[')) {
            depth++;
        } else if (ch == QLatin1Char(')') || ch == QLatin1Char('}') || ch == QLatin1Char(']')) {
            depth--;
        } else if (ch == QLatin1Char(',') && depth == 0) {
            parts.append(s.mid(start, i - start).trimmed());
            start = i + 1;
        }
    }
    const QString last = s.mid(start).trimmed();
    if (!last.isEmpty()) {
        parts.append(last);
    }
    return parts;
}

/* Integer constant expressions: literals, names from the symbol table, casts
 * to integer types, and the usual operators. */
class Evaluator {
public:
    explicit Evaluator(const SymbolTable &symbols) : m_symbols(symbols) {}

    bool eval(const QString &expr, qint64 *out, int depth = 0) const
    {
        if (depth > 16) {
            return false;
        }
        Parser p{expr, 0, this, depth, true};
        const qint64 v = p.binary(0);
        p.skip();
        if (!p.ok || p.pos != expr.size()) {
            return false;
        }
        *out = v;
        return true;
    }

private:
    struct Parser {
        const QString &s;
        int pos;
        const Evaluator *owner;
        int depth;
        bool ok;

        void skip()
        {
            while (pos < s.size() && s[pos].isSpace()) {
                pos++;
            }
        }
        bool take(const char *op)
        {
            skip();
            const QLatin1String l(op);
            if (s.mid(pos, l.size()) == l) {
                /* "<" must not swallow half of "<<", nor "|" half of "||". */
                const QChar next = pos + l.size() < s.size() ? s[pos + l.size()] : QChar();
                if (l.size() == 1 && (next == QLatin1Char(op[0])) && (op[0] == '|' || op[0] == '&')) {
                    return false;
                }
                pos += l.size();
                return true;
            }
            return false;
        }
        qint64 binary(int level)
        {
            static const char *const ops[][3] = {{"|", nullptr, nullptr}, {"^", nullptr, nullptr},
                                                 {"&", nullptr, nullptr}, {"<<", ">>", nullptr},
                                                 {"+", "-", nullptr},     {"*", "/", "%"}};
            if (level == 6) {
                return unary();
            }
            qint64 v = binary(level + 1);
            for (;;) {
                const char *hit = nullptr;
                for (int i = 0; i < 3 && ops[level][i] != nullptr; i++) {
                    if (take(ops[level][i])) {
                        hit = ops[level][i];
                        break;
                    }
                }
                if (hit == nullptr || !ok) {
                    return v;
                }
                const qint64 r = binary(level + 1);
                switch (hit[0]) {
                case '|': v |= r; break;
                case '^': v ^= r; break;
                case '&': v &= r; break;
                case '<': v = (r >= 0 && r < 63) ? v << r : 0; break;
                case '>': v = (r >= 0 && r < 63) ? v >> r : 0; break;
                case '+': v += r; break;
                case '-': v -= r; break;
                case '*': v *= r; break;
                case '/': if (r == 0) { ok = false; } else { v /= r; } break;
                case '%': if (r == 0) { ok = false; } else { v %= r; } break;
                }
            }
        }
        qint64 unary()
        {
            skip();
            if (take("-")) {
                return -unary();
            }
            if (take("+")) {
                return unary();
            }
            if (take("~")) {
                return ~unary();
            }
            if (pos < s.size() && s[pos] == QLatin1Char('(')) {
                static const QRegularExpression cast(QStringLiteral(
                    "\\G\\(\\s*(?:(?:const|unsigned|signed)\\s+)*(?:u?int\\d*_t|int|unsigned|signed|long|short|char|size_t)"
                    "(?:\\s+(?:int|long))*\\s*\\)"));
                const QRegularExpressionMatch m = cast.match(s, pos);
                if (m.hasMatch()) {
                    pos = int(m.capturedEnd());
                    return unary();
                }
                pos++;
                const qint64 v = binary(0);
                if (!take(")")) {
                    ok = false;
                }
                return v;
            }
            static const QRegularExpression number(QStringLiteral("\\G(0[xX][0-9a-fA-F]+|\\d+)[uUlL]*"));
            QRegularExpressionMatch m = number.match(s, pos);
            if (m.hasMatch()) {
                pos = int(m.capturedEnd());
                const QString digits = m.captured(1);
                bool good = false;
                const qint64 v = digits.startsWith(QLatin1String("0x"), Qt::CaseInsensitive)
                                     ? digits.mid(2).toLongLong(&good, 16)
                                     : digits.toLongLong(&good, (digits.size() > 1 && digits[0] == QLatin1Char('0')) ? 8 : 10);
                ok = ok && good;
                return v;
            }
            static const QRegularExpression name(QStringLiteral("\\G[A-Za-z_]\\w*"));
            m = name.match(s, pos);
            if (m.hasMatch()) {
                pos = int(m.capturedEnd());
                const auto it = owner->m_symbols.constFind(m.captured(0));
                qint64 v = 0;
                if (it == owner->m_symbols.constEnd() || !owner->eval(it->expr, &v, depth + 1)) {
                    ok = false;
                }
                return v;
            }
            ok = false;
            return 0;
        }
    };

    const SymbolTable &m_symbols;
};

/* The text of a string literal expression: "a" "b", a macro that expands to
 * one, or 0 / NULL / nullptr for none. False when it is something else. */
bool stringValue(const QString &exprIn, const SymbolTable &symbols, QString *out, int depth = 0)
{
    const QString expr = exprIn.trimmed();
    if (expr == QLatin1String("0") || expr == QLatin1String("NULL") || expr == QLatin1String("nullptr")) {
        out->clear();
        return true;
    }
    if (expr.startsWith(QLatin1Char('"'))) {
        QString text;
        int i = 0;
        while (i < expr.size()) {
            if (expr[i].isSpace()) {
                i++;
                continue;
            }
            if (expr[i] != QLatin1Char('"')) {
                return false;
            }
            i++;
            while (i < expr.size() && expr[i] != QLatin1Char('"')) {
                if (expr[i] == QLatin1Char('\\') && i + 1 < expr.size()) {
                    i++;
                }
                text += expr[i++];
            }
            i++;
        }
        *out = text;
        return true;
    }
    const auto it = symbols.constFind(expr);
    if (it != symbols.constEnd() && depth < 8) {
        return stringValue(it->expr, symbols, out, depth + 1);
    }
    return false;
}

struct SourceFile {
    QString relative;
    QString text;       /* comments stripped */
};

struct FoundArray {
    QString file;
    QString name;
    int line = 0;
    QList<Assembly> assemblies;
    QStringList notes;
    bool used = false;
};

void addSymbol(SymbolTable &symbols, const QString &name, const QString &expr, bool fromLibraryConfig)
{
    const auto it = symbols.constFind(name);
    if (it == symbols.constEnd() || (it->fromLibraryConfig && !fromLibraryConfig)) {
        symbols.insert(name, {expr.trimmed(), fromLibraryConfig});
    }
}

void collectSymbols(const SourceFile &f, SymbolTable &symbols)
{
    const bool lib = f.relative.endsWith(QLatin1String("eip_config.h"));

    static const QRegularExpression define(QStringLiteral("^[ \\t]*#[ \\t]*define[ \\t]+(\\w+)[ \\t]+([^\\n]+)$"),
                                           QRegularExpression::MultilineOption);
    for (QRegularExpressionMatchIterator it = define.globalMatch(f.text); it.hasNext();) {
        const QRegularExpressionMatch m = it.next();
        addSymbol(symbols, m.captured(1), m.captured(2), lib);
    }

    static const QRegularExpression enumRe(QStringLiteral("\\benum\\b[^{;()]*\\{([^{}]*)\\}"));
    for (QRegularExpressionMatchIterator it = enumRe.globalMatch(f.text); it.hasNext();) {
        QString previous;
        const QStringList items = splitTopLevel(it.next().captured(1));
        for (const QString &item : items) {
            const int eq = item.indexOf(QLatin1Char('='));
            const QString name = (eq < 0 ? item : item.left(eq)).trimmed();
            if (name.isEmpty()) {
                continue;
            }
            const QString expr = eq >= 0 ? item.mid(eq + 1)
                                         : previous.isEmpty() ? QStringLiteral("0")
                                                              : QStringLiteral("(%1)+1").arg(previous);
            addSymbol(symbols, name, expr, lib);
            previous = name;
        }
    }

    static const QRegularExpression constRe(QStringLiteral(
        "\\b(?:static\\s+)?const(?:expr)?\\s+(?:static\\s+)?(?:unsigned\\s+)?\\w+\\s+(\\w+)\\s*=\\s*([^;{}]+);"));
    for (QRegularExpressionMatchIterator it = constRe.globalMatch(f.text); it.hasNext();) {
        const QRegularExpressionMatch m = it.next();
        addSymbol(symbols, m.captured(1), m.captured(2), lib);
    }
}

/* One assembly from the six values of an EipAssemblyConfig, positional or designated. */
Assembly parseAssembly(const QStringList &fields, const SymbolTable &symbols, QStringList *notes)
{
    static const char *const order[] = {"instance", "direction", "size", "name", "initial", "implicit_only"};
    QHash<QString, QString> v;
    for (int i = 0; i < fields.size(); i++) {
        const QString f = fields[i];
        if (f.startsWith(QLatin1Char('.'))) {
            const int eq = f.indexOf(QLatin1Char('='));
            if (eq > 0) {
                v.insert(f.mid(1, eq - 1).trimmed(), f.mid(eq + 1).trimmed());
            }
        } else if (i < 6) {
            v.insert(QLatin1String(order[i]), f);
        }
    }

    const Evaluator ev(symbols);
    Assembly a;
    qint64 n = 0;
    if (ev.eval(v.value(QStringLiteral("instance")), &n)) {
        a.instance = int(n);
    } else {
        a.unresolvedInstance = v.value(QStringLiteral("instance"));
    }
    if (ev.eval(v.value(QStringLiteral("direction")), &n) && n >= 1 && n <= 3) {
        a.dir = AsmDir(int(n));
    } else {
        notes->append(QStringLiteral("direction \"%1\" not understood").arg(v.value(QStringLiteral("direction"))));
    }
    if (ev.eval(v.value(QStringLiteral("size"), QStringLiteral("0")), &n)) {
        a.size = int(n);
    } else {
        a.unresolvedSize = v.value(QStringLiteral("size"));
    }
    if (!stringValue(v.value(QStringLiteral("name"), QStringLiteral("0")), symbols, &a.name)) {
        a.name = v.value(QStringLiteral("name"));
    }
    if (a.name.isEmpty()) {
        a.name = QStringLiteral("Assembly %1").arg(a.instance);
    }
    if (ev.eval(v.value(QStringLiteral("implicit_only"), QStringLiteral("0")), &n)) {
        a.implicitOnly = (n != 0);
    }
    return a;
}

void setInt(std::optional<int> *field, const QString &expr, const SymbolTable &symbols, const char *what,
            QStringList *notes)
{
    qint64 n = 0;
    if (Evaluator(symbols).eval(expr, &n)) {
        *field = int(n);
    } else {
        notes->append(QStringLiteral("%1 \"%2\" could not be read").arg(QLatin1String(what), expr.trimmed()));
    }
}

/* Store one EipAdapterConfig field. assembliesRef receives the array named by .assemblies. */
void setConfigField(ScanCandidate *c, const QString &field, const QString &expr, const SymbolTable &symbols,
                    QString *assembliesRef)
{
    if (field == QLatin1String("vendor_id")) {
        setInt(&c->vendorId, expr, symbols, "vendor_id", &c->notes);
    } else if (field == QLatin1String("device_type")) {
        setInt(&c->deviceType, expr, symbols, "device_type", &c->notes);
    } else if (field == QLatin1String("product_code")) {
        setInt(&c->productCode, expr, symbols, "product_code", &c->notes);
    } else if (field == QLatin1String("revision_major")) {
        setInt(&c->revisionMajor, expr, symbols, "revision_major", &c->notes);
    } else if (field == QLatin1String("revision_minor")) {
        setInt(&c->revisionMinor, expr, symbols, "revision_minor", &c->notes);
    } else if (field == QLatin1String("product_name")) {
        QString name;
        if (stringValue(expr, symbols, &name)) {
            if (!name.isEmpty()) { /* "= 0" clears a pointer: it names nothing */
                c->productName = name;
            }
        } else {
            c->notes.append(QStringLiteral("product_name \"%1\" could not be read").arg(expr.trimmed()));
        }
    } else if (field == QLatin1String("assemblies")) {
        static const QRegularExpression ident(QStringLiteral("[A-Za-z_]\\w*"));
        *assembliesRef = ident.match(expr).captured(0);
    }
}

void describe(ScanCandidate *c)
{
    QString what;
    if (c->builtInTestSet) {
        what = QStringLiteral("built-in test set");
    } else if (c->productName && !c->productName->isEmpty()) {
        what = QLatin1Char('"') + *c->productName + QLatin1Char('"');
    } else if (c->vendorId) {
        what = QStringLiteral("vendor %1").arg(*c->vendorId);
    } else {
        what = QStringLiteral("no identity");
    }
    c->label = QStringLiteral("%1:%2  -  %3, %4 assembl%5")
                   .arg(c->file).arg(c->line).arg(what).arg(c->assemblies.size())
                   .arg(c->assemblies.size() == 1 ? QStringLiteral("y") : QStringLiteral("ies"));
}

int score(const ScanCandidate &c)
{
    int s = 0;
    s += c.identityComplete() ? 4 : (c.vendorId ? 1 : 0);
    s += c.assemblies.isEmpty() ? 0 : 4;
    s -= c.builtInTestSet ? 3 : 0;
    s -= c.file.contains(QLatin1String("test"), Qt::CaseInsensitive) ? 2 : 0;
    return s;
}

bool skipDir(const QString &name)
{
    static const QStringList exact = {QStringLiteral("node_modules"), QStringLiteral("CMakeFiles"), QStringLiteral("obj"),
                                      QStringLiteral("out"),          QStringLiteral("Debug"),      QStringLiteral("Release"),
                                      QStringLiteral("x64")};
    return name.startsWith(QLatin1Char('.')) || name.startsWith(QLatin1String("build"), Qt::CaseInsensitive) ||
           name.startsWith(QLatin1String("cmake-build")) || exact.contains(name);
}

void walk(const QDir &dir, const QDir &root, QStringList *sources, QStringList *buildFiles, int depth)
{
    static const QStringList sourceExt = {QStringLiteral("c"),   QStringLiteral("cpp"), QStringLiteral("cc"),
                                          QStringLiteral("cxx"), QStringLiteral("h"),   QStringLiteral("hpp"),
                                          QStringLiteral("ino")};
    if (depth > 12) {
        return;
    }
    const QFileInfoList entries = dir.entryInfoList(QDir::Files | QDir::Dirs | QDir::NoDotAndDotDot | QDir::NoSymLinks,
                                                    QDir::Name);
    for (const QFileInfo &e : entries) {
        if (e.isDir()) {
            if (!skipDir(e.fileName())) {
                walk(QDir(e.absoluteFilePath()), root, sources, buildFiles, depth + 1);
            }
        } else if (e.size() <= 4 * 1024 * 1024) {
            const QString name = e.fileName();
            if (sourceExt.contains(e.suffix().toLower())) {
                sources->append(e.absoluteFilePath());
            } else if (name == QLatin1String("CMakeLists.txt") || name.compare(QLatin1String("makefile"), Qt::CaseInsensitive) == 0 ||
                       e.suffix() == QLatin1String("mk") || e.suffix() == QLatin1String("cmake") ||
                       name == QLatin1String("platformio.ini")) {
                buildFiles->append(e.absoluteFilePath());
            }
        }
    }
}

QString readText(const QString &path)
{
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) {
        return QString();
    }
    return QString::fromLatin1(f.readAll());
}

} /* namespace */

ScanResult scanProject(const QString &rootPath)
{
    ScanResult result;
    const QDir root(rootPath);
    result.root = root.absolutePath();

    QStringList sourcePaths;
    QStringList buildPaths;
    walk(root, root, &sourcePaths, &buildPaths, 0);

    SymbolTable symbols;
    addSymbol(symbols, QStringLiteral("EIP_ASM_CONSUMED"), QStringLiteral("1"), true);
    addSymbol(symbols, QStringLiteral("EIP_ASM_PRODUCED"), QStringLiteral("2"), true);
    addSymbol(symbols, QStringLiteral("EIP_ASM_CONFIG"), QStringLiteral("3"), true);
    addSymbol(symbols, QStringLiteral("true"), QStringLiteral("1"), true);
    addSymbol(symbols, QStringLiteral("false"), QStringLiteral("0"), true);

    QList<SourceFile> files;
    for (const QString &path : sourcePaths) {
        SourceFile f;
        f.relative = root.relativeFilePath(path);
        f.text = stripComments(readText(path));
        collectSymbols(f, symbols);
        files.append(f);
    }
    result.filesScanned = int(files.size());

    /* -DEIP_X=n on a compiler command line beats any header. */
    static const QRegularExpression dashD(QStringLiteral("\\b(EIP_[A-Z0-9_]+)=(\\d+)\\b"));
    for (const QString &path : buildPaths) {
        const QString text = readText(path);
        for (QRegularExpressionMatchIterator it = dashD.globalMatch(text); it.hasNext();) {
            const QRegularExpressionMatch m = it.next();
            symbols.insert(m.captured(1), {m.captured(2), false});
        }
    }
    {
        const Evaluator ev(symbols);
        qint64 n = 0;
        if (ev.eval(QStringLiteral("EIP_ADAPTER_MAX_ASSEMBLIES"), &n) && n > 0) {
            result.limitAssemblies = int(n);
        }
        if (ev.eval(QStringLiteral("EIP_ADAPTER_DATA_POOL"), &n) && n > 0) {
            result.limitPool = int(n);
        }
        if (ev.eval(QStringLiteral("EIP_ADAPTER_MAX_CLIENTS"), &n) && n > 0) {
            result.maxClients = int(n);
        }
    }

    /* Every EipAssemblyConfig array in the tree. */
    QList<FoundArray> arrays;
    static const QRegularExpression arrayRe(QStringLiteral("\\bEipAssemblyConfig\\s+(\\w+)\\s*\\[[^\\]]*\\]\\s*=\\s*\\{"));
    for (const SourceFile &f : files) {
        for (QRegularExpressionMatchIterator it = arrayRe.globalMatch(f.text); it.hasNext();) {
            const QRegularExpressionMatch m = it.next();
            const int open = int(m.capturedEnd()) - 1;
            const int close = matchBracket(f.text, open);
            if (close < 0) {
                continue;
            }
            FoundArray a;
            a.file = f.relative;
            a.name = m.captured(1);
            a.line = lineOf(f.text, int(m.capturedStart()));
            const QStringList elements = splitTopLevel(f.text.mid(open + 1, close - open - 1));
            for (const QString &element : elements) {
                if (element.startsWith(QLatin1Char('{')) && element.endsWith(QLatin1Char('}'))) {
                    a.assemblies.append(parseAssembly(splitTopLevel(element.mid(1, element.size() - 2)), symbols, &a.notes));
                }
            }
            arrays.append(a);
        }
    }

    auto findArray = [&](const QString &file, const QString &name) -> FoundArray * {
        FoundArray *other = nullptr;
        FoundArray *onlyInFile = nullptr;
        int inFile = 0;
        for (FoundArray &a : arrays) {
            if (a.file == file) {
                inFile++;
                onlyInFile = &a;
            }
            if (!name.isEmpty() && a.name == name) {
                if (a.file == file) {
                    return &a;
                }
                other = &a;
            }
        }
        if (other != nullptr) {
            return other;
        }
        return (name.isEmpty() && inFile == 1) ? onlyInFile : nullptr;
    };

    static const QRegularExpression varRe(QStringLiteral("\\bEipAdapterConfig\\s*[*&]?\\s*(\\w+)\\s*(=\\s*\\{)?"));
    static const QRegularExpression setIdentityRe(QStringLiteral("\\beip_adapter_set_identity\\s*\\("));
    static const QRegularExpression addAssemblyRe(QStringLiteral("\\beip_adapter_add_assembly\\s*\\("));
    static const QRegularExpression clearRe(QStringLiteral("\\beip_adapter_clear_assemblies\\s*\\("));
    static const QRegularExpression startNullRe(
        QStringLiteral("\\b(?:eip_adapter_start\\s*\\(|eip_server_init\\s*\\([^,()]+,)\\s*(?:0|NULL|nullptr)\\s*\\)"));
    static const char *const kFields = "vendor_id|device_type|product_code|revision_major|revision_minor|product_name|assemblies";

    for (const SourceFile &f : files) {
        /* EipAdapterConfig variables and what is assigned to them. */
        QStringList seenVars;
        for (QRegularExpressionMatchIterator it = varRe.globalMatch(f.text); it.hasNext();) {
            const QRegularExpressionMatch m = it.next();
            const QString var = m.captured(1);
            if (seenVars.contains(var)) {
                continue;
            }
            seenVars.append(var);
            ScanCandidate c;
            c.file = f.relative;
            c.line = lineOf(f.text, int(m.capturedStart()));
            QString arrayName;
            bool any = false;

            if (!m.captured(2).isEmpty()) {
                const int open = int(m.capturedEnd()) - 1;
                const int close = matchBracket(f.text, open);
                if (close > open) {
                    static const char *const order[] = {"vendor_id", "device_type", "product_code", "revision_major",
                                                        "revision_minor", "product_name"};
                    const QStringList items = splitTopLevel(f.text.mid(open + 1, close - open - 1));
                    for (int i = 0; i < items.size(); i++) {
                        const QString item = items[i];
                        const int eq = item.indexOf(QLatin1Char('='));
                        if (item.startsWith(QLatin1Char('.')) && eq > 0) {
                            setConfigField(&c, item.mid(1, eq - 1).trimmed(), item.mid(eq + 1), symbols, &arrayName);
                            any = true;
                        } else if (i < 6 && items.size() > 1) {
                            setConfigField(&c, QLatin1String(order[i]), item, symbols, &arrayName);
                            any = true;
                        }
                    }
                }
            }
            const QRegularExpression assign(QStringLiteral("\\b%1\\s*(?:\\.|->)\\s*(%2)\\s*=(?!=)\\s*([^;]+);")
                                                .arg(QRegularExpression::escape(var), QLatin1String(kFields)));
            for (QRegularExpressionMatchIterator at = assign.globalMatch(f.text); at.hasNext();) {
                const QRegularExpressionMatch am = at.next();
                setConfigField(&c, am.captured(1), am.captured(2), symbols, &arrayName);
                any = true;
            }
            if (!any) {
                continue;
            }
            if (FoundArray *a = findArray(f.relative, arrayName)) {
                c.assemblies = a->assemblies;
                c.notes += a->notes;
                a->used = true;
            } else if (!arrayName.isEmpty()) {
                c.notes.append(QStringLiteral("assembly array \"%1\" was not found").arg(arrayName));
            }
            if (!c.vendorId && !c.productName && !c.productCode && c.assemblies.isEmpty()) {
                continue; /* a variable that only passes a config along */
            }
            result.candidates.append(c);
        }

        /* The sans-I/O API: add_assembly calls, grouped between clear_assemblies calls,
         * with the nearest set_identity call before each group. */
        QList<int> clears;
        for (QRegularExpressionMatchIterator it = clearRe.globalMatch(f.text); it.hasNext();) {
            clears.append(int(it.next().capturedStart()));
        }
        struct Identity {
            int offset;
            QStringList args;
        };
        QList<Identity> identities;
        for (QRegularExpressionMatchIterator it = setIdentityRe.globalMatch(f.text); it.hasNext();) {
            const QRegularExpressionMatch m = it.next();
            const int open = int(m.capturedEnd()) - 1;
            const int close = matchBracket(f.text, open);
            const QString inner = close > open ? f.text.mid(open + 1, close - open - 1) : QString();
            if (!inner.contains(QLatin1String("uint16_t"))) { /* not the prototype */
                identities.append({int(m.capturedStart()), splitTopLevel(inner)});
            }
        }
        QHash<int, ScanCandidate> groups; /* keyed by the number of clears before the call */
        QList<int> groupOrder;
        for (QRegularExpressionMatchIterator it = addAssemblyRe.globalMatch(f.text); it.hasNext();) {
            const QRegularExpressionMatch m = it.next();
            const int open = int(m.capturedEnd()) - 1;
            const int close = matchBracket(f.text, open);
            const QString inner = close > open ? f.text.mid(open + 1, close - open - 1) : QString();
            if (inner.isEmpty() || inner.contains(QLatin1String("uint16_t"))) {
                continue;
            }
            const int offset = int(m.capturedStart());
            const int key = int(std::count_if(clears.begin(), clears.end(), [&](int c) { return c < offset; }));
            if (!groups.contains(key)) {
                ScanCandidate c;
                c.file = f.relative;
                c.line = lineOf(f.text, offset);
                for (const Identity &id : identities) {
                    if (id.offset < offset && id.args.size() >= 7) {
                        static const char *const order[] = {"vendor_id", "device_type", "product_code", "revision_major",
                                                            "revision_minor", "product_name"};
                        QString unused;
                        ScanCandidate fresh = c;
                        fresh.notes.clear();
                        for (int i = 0; i < 6; i++) {
                            setConfigField(&fresh, QLatin1String(order[i]), id.args[i + 1], symbols, &unused);
                        }
                        if (fresh.vendorId) { /* else the call forwards variables: nothing to learn */
                            c = fresh;
                        }
                    }
                }
                groups.insert(key, c);
                groupOrder.append(key);
            }
            QStringList args = splitTopLevel(inner);
            if (args.size() >= 5) {
                args.removeFirst(); /* the adapter pointer */
                ScanCandidate &c = groups[key];
                QStringList notes;
                const Assembly a = parseAssembly(args.mid(0, 5), symbols, &notes);
                /* A call that passes variables through (the library's own loop over a
                 * config) defines no assembly. */
                if (a.unresolvedInstance.isEmpty() || a.dir != AsmDir::Unknown) {
                    c.assemblies.append(a);
                    c.notes += notes;
                }
            }
        }
        for (int key : groupOrder) {
            if (!groups.value(key).assemblies.isEmpty()) {
                result.candidates.append(groups.value(key));
            }
        }

        const QRegularExpressionMatch nm = startNullRe.match(f.text);
        if (nm.hasMatch()) {
            /* The identity and assemblies of eip_adapter_init. */
            ScanCandidate c;
            c.file = f.relative;
            c.line = lineOf(f.text, int(nm.capturedStart()));
            c.builtInTestSet = true;
            c.vendorId = 65000;
            c.deviceType = 12;
            c.productCode = 1;
            c.revisionMajor = 1;
            c.revisionMinor = 1;
            c.productName = QStringLiteral("EIP Test Adapter");
            auto add = [&](int instance, AsmDir dir, int size, const char *name) {
                Assembly a;
                a.instance = instance;
                a.dir = dir;
                a.size = size;
                a.name = QLatin1String(name);
                c.assemblies.append(a);
            };
            add(100, AsmDir::Consumed, 8, "Output");
            add(101, AsmDir::Produced, 8, "Input");
            add(102, AsmDir::Produced, 4, "Status");
            add(150, AsmDir::Config, 4, "Config");
            result.candidates.append(c);
        }
    }

    /* Arrays no EipAdapterConfig points at still describe an adapter. */
    for (const FoundArray &a : arrays) {
        if (!a.used && !a.assemblies.isEmpty()) {
            ScanCandidate c;
            c.file = a.file;
            c.line = a.line;
            c.assemblies = a.assemblies;
            c.notes = a.notes;
            c.notes.append(QStringLiteral("no EipAdapterConfig refers to this array: enter the identity by hand"));
            result.candidates.append(c);
        }
    }

    for (ScanCandidate &c : result.candidates) {
        for (const Assembly &a : c.assemblies) {
            if (!a.unresolvedInstance.isEmpty()) {
                c.notes.append(QStringLiteral("assembly instance \"%1\" could not be read").arg(a.unresolvedInstance));
            }
            if (!a.unresolvedSize.isEmpty()) {
                c.notes.append(QStringLiteral("size \"%1\" of assembly %2 could not be read").arg(a.unresolvedSize).arg(a.instance));
            }
        }
        describe(&c);
    }
    std::stable_sort(result.candidates.begin(), result.candidates.end(),
                     [](const ScanCandidate &a, const ScanCandidate &b) { return score(a) > score(b); });
    return result;
}

void applyCandidate(const ScanResult &result, const ScanCandidate &c, EdsModel &m)
{
    if (c.vendorId) {
        m.vendCode = *c.vendorId;
    }
    if (c.deviceType) {
        m.prodType = *c.deviceType;
        const QString name = deviceTypeName(m.prodType);
        if (!name.isEmpty()) {
            m.prodTypeStr = name;
        }
    }
    if (c.productCode) {
        m.prodCode = *c.productCode;
    }
    if (c.revisionMajor) {
        m.majRev = *c.revisionMajor;
    }
    if (c.revisionMinor) {
        m.minRev = *c.revisionMinor;
    }
    if (c.productName) {
        m.prodName = c.productName->left(31); /* what eip_adapter_set_identity keeps */
        m.catalog = m.prodName;
        m.descText = m.prodName + QStringLiteral(" EtherNet/IP adapter");
    }
    m.assemblies = c.assemblies;
    m.connections.clear();
    m.limitAssemblies = result.limitAssemblies;
    m.limitPool = result.limitPool;
    m.sourceNote = QStringLiteral("%1:%2").arg(c.file).arg(c.line);
    rebuildConnections(m);
}
