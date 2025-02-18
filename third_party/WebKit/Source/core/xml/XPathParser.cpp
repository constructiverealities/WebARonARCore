/*
 * Copyright 2005 Maksim Orlovich <maksim@kde.org>
 * Copyright (C) 2006 Apple Computer, Inc.
 * Copyright (C) 2007 Alexey Proskuryakov <ap@webkit.org>
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 *
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR ``AS IS'' AND ANY EXPRESS OR
 * IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES
 * OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED.
 * IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR ANY DIRECT, INDIRECT,
 * INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT
 * NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 * DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 * THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF
 * THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#include "core/xml/XPathParser.h"

#include "bindings/core/v8/ExceptionState.h"
#include "core/XPathGrammar.h"
#include "core/dom/ExceptionCode.h"
#include "core/xml/XPathEvaluator.h"
#include "core/xml/XPathNSResolver.h"
#include "core/xml/XPathPath.h"
#include "wtf/PtrUtil.h"
#include "wtf/StdLibExtras.h"
#include "wtf/text/StringHash.h"

using namespace blink;
using namespace WTF;
using namespace Unicode;
using namespace XPath;

Parser* Parser::currentParser = nullptr;

enum XMLCat { NameStart, NameCont, NotPartOfName };

typedef HashMap<String, Step::Axis> AxisNamesMap;

static XMLCat charCat(UChar aChar) {
  // might need to add some special cases from the XML spec.

  if (aChar == '_')
    return NameStart;

  if (aChar == '.' || aChar == '-')
    return NameCont;
  CharCategory category = Unicode::category(aChar);
  if (category & (Letter_Uppercase | Letter_Lowercase | Letter_Other |
                  Letter_Titlecase | Number_Letter))
    return NameStart;
  if (category & (Mark_NonSpacing | Mark_SpacingCombining | Mark_Enclosing |
                  Letter_Modifier | Number_DecimalDigit))
    return NameCont;
  return NotPartOfName;
}

static void setUpAxisNamesMap(AxisNamesMap& axisNames) {
  struct AxisName {
    const char* name;
    Step::Axis axis;
  };
  const AxisName axisNameList[] = {
      {"ancestor", Step::AncestorAxis},
      {"ancestor-or-self", Step::AncestorOrSelfAxis},
      {"attribute", Step::AttributeAxis},
      {"child", Step::ChildAxis},
      {"descendant", Step::DescendantAxis},
      {"descendant-or-self", Step::DescendantOrSelfAxis},
      {"following", Step::FollowingAxis},
      {"following-sibling", Step::FollowingSiblingAxis},
      {"namespace", Step::NamespaceAxis},
      {"parent", Step::ParentAxis},
      {"preceding", Step::PrecedingAxis},
      {"preceding-sibling", Step::PrecedingSiblingAxis},
      {"self", Step::SelfAxis}};
  for (const auto& axisName : axisNameList)
    axisNames.set(axisName.name, axisName.axis);
}

static bool isAxisName(const String& name, Step::Axis& type) {
  DEFINE_STATIC_LOCAL(AxisNamesMap, axisNames, ());

  if (axisNames.isEmpty())
    setUpAxisNamesMap(axisNames);

  AxisNamesMap::iterator it = axisNames.find(name);
  if (it == axisNames.end())
    return false;
  type = it->value;
  return true;
}

static bool isNodeTypeName(const String& name) {
  DEFINE_STATIC_LOCAL(HashSet<String>, nodeTypeNames,
                      ({
                          "comment", "text", "processing-instruction", "node",
                      }));
  return nodeTypeNames.contains(name);
}

// Returns whether the current token can possibly be a binary operator, given
// the previous token. Necessary to disambiguate some of the operators
// (* (multiply), div, and, or, mod) in the [32] Operator rule
// (check http://www.w3.org/TR/xpath#exprlex).
bool Parser::isBinaryOperatorContext() const {
  switch (m_lastTokenType) {
    case 0:
    case '@':
    case AXISNAME:
    case '(':
    case '[':
    case ',':
    case AND:
    case OR:
    case MULOP:
    case '/':
    case SLASHSLASH:
    case '|':
    case PLUS:
    case MINUS:
    case EQOP:
    case RELOP:
      return false;
    default:
      return true;
  }
}

void Parser::skipWS() {
  while (m_nextPos < m_data.length() && isSpaceOrNewline(m_data[m_nextPos]))
    ++m_nextPos;
}

Token Parser::makeTokenAndAdvance(int code, int advance) {
  m_nextPos += advance;
  return Token(code);
}

Token Parser::makeTokenAndAdvance(int code,
                                  NumericOp::Opcode val,
                                  int advance) {
  m_nextPos += advance;
  return Token(code, val);
}

Token Parser::makeTokenAndAdvance(int code, EqTestOp::Opcode val, int advance) {
  m_nextPos += advance;
  return Token(code, val);
}

// Returns next char if it's there and interesting, 0 otherwise
char Parser::peekAheadHelper() {
  if (m_nextPos + 1 >= m_data.length())
    return 0;
  UChar next = m_data[m_nextPos + 1];
  if (next >= 0xff)
    return 0;
  return next;
}

char Parser::peekCurHelper() {
  if (m_nextPos >= m_data.length())
    return 0;
  UChar next = m_data[m_nextPos];
  if (next >= 0xff)
    return 0;
  return next;
}

Token Parser::lexString() {
  UChar delimiter = m_data[m_nextPos];
  int startPos = m_nextPos + 1;

  for (m_nextPos = startPos; m_nextPos < m_data.length(); ++m_nextPos) {
    if (m_data[m_nextPos] == delimiter) {
      String value = m_data.substring(startPos, m_nextPos - startPos);
      if (value.isNull())
        value = "";
      ++m_nextPos;  // Consume the char.
      return Token(LITERAL, value);
    }
  }

  // Ouch, went off the end -- report error.
  return Token(XPATH_ERROR);
}

Token Parser::lexNumber() {
  int startPos = m_nextPos;
  bool seenDot = false;

  // Go until end or a non-digits character.
  for (; m_nextPos < m_data.length(); ++m_nextPos) {
    UChar aChar = m_data[m_nextPos];
    if (aChar >= 0xff)
      break;

    if (aChar < '0' || aChar > '9') {
      if (aChar == '.' && !seenDot)
        seenDot = true;
      else
        break;
    }
  }

  return Token(NUMBER, m_data.substring(startPos, m_nextPos - startPos));
}

bool Parser::lexNCName(String& name) {
  int startPos = m_nextPos;
  if (m_nextPos >= m_data.length())
    return false;

  if (charCat(m_data[m_nextPos]) != NameStart)
    return false;

  // Keep going until we get a character that's not good for names.
  for (; m_nextPos < m_data.length(); ++m_nextPos) {
    if (charCat(m_data[m_nextPos]) == NotPartOfName)
      break;
  }

  name = m_data.substring(startPos, m_nextPos - startPos);
  return true;
}

bool Parser::lexQName(String& name) {
  String n1;
  if (!lexNCName(n1))
    return false;

  skipWS();

  // If the next character is :, what we just got it the prefix, if not,
  // it's the whole thing.
  if (peekAheadHelper() != ':') {
    name = n1;
    return true;
  }

  String n2;
  if (!lexNCName(n2))
    return false;

  name = n1 + ":" + n2;
  return true;
}

Token Parser::nextTokenInternal() {
  skipWS();

  if (m_nextPos >= m_data.length())
    return Token(0);

  char code = peekCurHelper();
  switch (code) {
    case '(':
    case ')':
    case '[':
    case ']':
    case '@':
    case ',':
    case '|':
      return makeTokenAndAdvance(code);
    case '\'':
    case '\"':
      return lexString();
    case '0':
    case '1':
    case '2':
    case '3':
    case '4':
    case '5':
    case '6':
    case '7':
    case '8':
    case '9':
      return lexNumber();
    case '.': {
      char next = peekAheadHelper();
      if (next == '.')
        return makeTokenAndAdvance(DOTDOT, 2);
      if (next >= '0' && next <= '9')
        return lexNumber();
      return makeTokenAndAdvance('.');
    }
    case '/':
      if (peekAheadHelper() == '/')
        return makeTokenAndAdvance(SLASHSLASH, 2);
      return makeTokenAndAdvance('/');
    case '+':
      return makeTokenAndAdvance(PLUS);
    case '-':
      return makeTokenAndAdvance(MINUS);
    case '=':
      return makeTokenAndAdvance(EQOP, EqTestOp::OpcodeEqual);
    case '!':
      if (peekAheadHelper() == '=')
        return makeTokenAndAdvance(EQOP, EqTestOp::OpcodeNotEqual, 2);
      return Token(XPATH_ERROR);
    case '<':
      if (peekAheadHelper() == '=')
        return makeTokenAndAdvance(RELOP, EqTestOp::OpcodeLessOrEqual, 2);
      return makeTokenAndAdvance(RELOP, EqTestOp::OpcodeLessThan);
    case '>':
      if (peekAheadHelper() == '=')
        return makeTokenAndAdvance(RELOP, EqTestOp::OpcodeGreaterOrEqual, 2);
      return makeTokenAndAdvance(RELOP, EqTestOp::OpcodeGreaterThan);
    case '*':
      if (isBinaryOperatorContext())
        return makeTokenAndAdvance(MULOP, NumericOp::OP_Mul);
      ++m_nextPos;
      return Token(NAMETEST, "*");
    case '$': {  // $ QName
      m_nextPos++;
      String name;
      if (!lexQName(name))
        return Token(XPATH_ERROR);
      return Token(VARIABLEREFERENCE, name);
    }
  }

  String name;
  if (!lexNCName(name))
    return Token(XPATH_ERROR);

  skipWS();
  // If we're in an operator context, check for any operator names
  if (isBinaryOperatorContext()) {
    if (name == "and")  // ### hash?
      return Token(AND);
    if (name == "or")
      return Token(OR);
    if (name == "mod")
      return Token(MULOP, NumericOp::OP_Mod);
    if (name == "div")
      return Token(MULOP, NumericOp::OP_Div);
  }

  // See whether we are at a :
  if (peekCurHelper() == ':') {
    m_nextPos++;
    // Any chance it's an axis name?
    if (peekCurHelper() == ':') {
      m_nextPos++;

      // It might be an axis name.
      Step::Axis axis;
      if (isAxisName(name, axis))
        return Token(AXISNAME, axis);
      // Ugh, :: is only valid in axis names -> error
      return Token(XPATH_ERROR);
    }

    // Seems like this is a fully qualified qname, or perhaps the * modified
    // one from NameTest
    skipWS();
    if (peekCurHelper() == '*') {
      m_nextPos++;
      return Token(NAMETEST, name + ":*");
    }

    // Make a full qname.
    String n2;
    if (!lexNCName(n2))
      return Token(XPATH_ERROR);

    name = name + ":" + n2;
  }

  skipWS();
  if (peekCurHelper() == '(') {
    // Note: we don't swallow the ( here!

    // Either node type of function name
    if (isNodeTypeName(name)) {
      if (name == "processing-instruction")
        return Token(PI, name);

      return Token(NODETYPE, name);
    }
    // Must be a function name.
    return Token(FUNCTIONNAME, name);
  }

  // At this point, it must be NAMETEST.
  return Token(NAMETEST, name);
}

Token Parser::nextToken() {
  Token toRet = nextTokenInternal();
  m_lastTokenType = toRet.type;
  return toRet;
}

Parser::Parser() {
  reset(String());
}

Parser::~Parser() {}

void Parser::reset(const String& data) {
  m_nextPos = 0;
  m_data = data;
  m_lastTokenType = 0;

  m_topExpr = nullptr;
  m_gotNamespaceError = false;
}

int Parser::lex(void* data) {
  YYSTYPE* yylval = static_cast<YYSTYPE*>(data);
  Token tok = nextToken();

  switch (tok.type) {
    case AXISNAME:
      yylval->axis = tok.axis;
      break;
    case MULOP:
      yylval->numop = tok.numop;
      break;
    case RELOP:
    case EQOP:
      yylval->eqop = tok.eqop;
      break;
    case NODETYPE:
    case PI:
    case FUNCTIONNAME:
    case LITERAL:
    case VARIABLEREFERENCE:
    case NUMBER:
    case NAMETEST:
      yylval->str = new String(tok.str);
      registerString(yylval->str);
      break;
  }

  return tok.type;
}

bool Parser::expandQName(const String& qName,
                         AtomicString& localName,
                         AtomicString& namespaceURI) {
  size_t colon = qName.find(':');
  if (colon != kNotFound) {
    if (!m_resolver)
      return false;
    namespaceURI = m_resolver->lookupNamespaceURI(qName.left(colon));
    if (namespaceURI.isNull())
      return false;
    localName = AtomicString(qName.substring(colon + 1));
  } else {
    localName = AtomicString(qName);
  }

  return true;
}

Expression* Parser::parseStatement(const String& statement,
                                   XPathNSResolver* resolver,
                                   ExceptionState& exceptionState) {
  reset(statement);

  m_resolver = resolver;

  Parser* oldParser = currentParser;
  currentParser = this;
  int parseError = xpathyyparse(this);
  currentParser = oldParser;

  if (parseError) {
    m_strings.clear();

    m_topExpr = nullptr;

    if (m_gotNamespaceError)
      exceptionState.throwDOMException(
          NamespaceError,
          "The string '" + statement + "' contains unresolvable namespaces.");
    else
      exceptionState.throwDOMException(
          SyntaxError,
          "The string '" + statement + "' is not a valid XPath expression.");
    return nullptr;
  }
  DCHECK_EQ(m_strings.size(), 0u);
  Expression* result = m_topExpr;
  m_topExpr = nullptr;

  return result;
}

void Parser::registerString(String* s) {
  if (!s)
    return;

  DCHECK(!m_strings.contains(s));
  m_strings.add(WTF::wrapUnique(s));
}

void Parser::deleteString(String* s) {
  if (!s)
    return;

  DCHECK(m_strings.contains(s));
  m_strings.remove(s);
}
