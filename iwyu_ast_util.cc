//===--- iwyu_ast_util.cc - clang-AST utilities for include-what-you-use --===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//

// Utilities that make it easier to work with Clang's AST.

#include "iwyu_ast_util.h"

#include <set>                          // for set
#include <string>                       // for string, operator+, etc
#include <utility>                      // for pair

#include "clang/AST/ASTContext.h"
#include "clang/AST/ASTDumper.h"
#include "clang/AST/CanonicalType.h"
#include "clang/AST/Decl.h"
#include "clang/AST/DeclBase.h"
#include "clang/AST/DeclCXX.h"
#include "clang/AST/DeclTemplate.h"
#include "clang/AST/Expr.h"
#include "clang/AST/ExprCXX.h"
#include "clang/AST/NestedNameSpecifier.h"
#include "clang/AST/OperationKinds.h"
#include "clang/AST/PrettyPrinter.h"
#include "clang/AST/RecursiveASTVisitor.h"
#include "clang/AST/Redeclarable.h"
#include "clang/AST/Stmt.h"
#include "clang/AST/TemplateBase.h"
#include "clang/AST/TemplateName.h"
#include "clang/AST/Type.h"
#include "clang/AST/TypeLoc.h"
#include "clang/Basic/Builtins.h"
#include "clang/Basic/FileEntry.h"
#include "clang/Basic/IdentifierTable.h"
#include "clang/Basic/SourceLocation.h"
#include "clang/Basic/SourceManager.h"
#include "clang/Basic/Specifiers.h"
#include "clang/Sema/EnterExpressionEvaluationContext.h"
#include "clang/Sema/Initialization.h"
#include "clang/Sema/Ownership.h"
#include "clang/Sema/Sema.h"
#include "iwyu_globals.h"
#include "iwyu_location_util.h"
#include "iwyu_path_util.h"
#include "iwyu_port.h"  // for CHECK_
#include "iwyu_stl_util.h"
#include "iwyu_verrs.h"
#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/PointerUnion.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/Support/Casting.h"

// TODO: Clean out pragmas as IWYU improves.
// IWYU pragma: no_include "clang/AST/StmtIterator.h"
// IWYU pragma: no_include "clang/Basic/CustomizableOptional.h"

using clang::ASTContext;
using clang::ASTDumper;
using clang::ArrayType;
using clang::BlockPointerType;
using clang::CXXConstructExpr;
using clang::CXXConstructorDecl;
using clang::CXXDefaultArgExpr;
using clang::CXXDeleteExpr;
using clang::CXXDependentScopeMemberExpr;
using clang::CXXDestructorDecl;
using clang::CXXMethodDecl;
using clang::CXXNewExpr;
using clang::CXXRecordDecl;
using clang::CallExpr;
using clang::ClassTemplateDecl;
using clang::ClassTemplatePartialSpecializationDecl;
using clang::ClassTemplateSpecializationDecl;
using clang::Decl;
using clang::DeclContext;
using clang::DeclRefExpr;
using clang::DeclaratorDecl;
using clang::DependentNameType;
using clang::DependentScopeDeclRefExpr;
using clang::DependentTemplateName;
using clang::DependentTemplateSpecializationType;
using clang::ElaboratedType;
using clang::ElaboratedTypeKeyword;
using clang::EnterExpressionEvaluationContext;
using clang::EnumDecl;
using clang::EnumType;
using clang::ExplicitCastExpr;
using clang::Expr;
using clang::ExprResult;
using clang::ExprWithCleanups;
using clang::FunctionDecl;
using clang::FunctionTemplateDecl;
using clang::FunctionTemplateSpecializationInfo;
using clang::FunctionType;
using clang::IdentifierInfo;
using clang::ImplicitCastExpr;
using clang::InitializationKind;
using clang::InitializationSequence;
using clang::InitializedEntity;
using clang::InjectedClassNameType;
using clang::LValueReferenceType;
using clang::MemberExpr;
using clang::MemberPointerType;
using clang::NamedDecl;
using clang::NamespaceDecl;
using clang::NestedNameSpecifier;
using clang::ObjCObjectType;
using clang::OpaqueValueExpr;
using clang::OptionalFileEntryRef;
using clang::OverloadExpr;
using clang::PointerType;
using clang::PrintingPolicy;
using clang::QualType;
using clang::QualifiedTemplateName;
using clang::RecordDecl;
using clang::RecordType;
using clang::RecursiveASTVisitor;
using clang::Redeclarable;
using clang::ReferenceType;
using clang::Sema;
using clang::SourceLocation;
using clang::SourceManager;
using clang::SourceRange;
using clang::Stmt;
using clang::SubstTemplateTypeParmType;
using clang::TagDecl;
using clang::TagType;
using clang::TemplateArgument;
using clang::TemplateArgumentList;
using clang::TemplateArgumentListInfo;
using clang::TemplateArgumentLoc;
using clang::TemplateDecl;
using clang::TemplateName;
using clang::TemplateParameterList;
using clang::TemplateSpecializationKind;
using clang::TemplateSpecializationType;
using clang::TemplateTypeParmDecl;
using clang::TranslationUnitDecl;
using clang::Type;
using clang::TypeAliasTemplateDecl;
using clang::TypeDecl;
using clang::TypeLoc;
using clang::TypedefNameDecl;
using clang::TypedefType;
using clang::UnaryOperator;
using clang::UnresolvedLookupExpr;
using clang::UsingDirectiveDecl;
using clang::ValueDecl;
using clang::VarDecl;
using clang::VarTemplateSpecializationDecl;
using llvm::ArrayRef;
using llvm::PointerUnion;
using llvm::cast;
using llvm::dyn_cast;
using llvm::dyn_cast_or_null;
using llvm::errs;
using llvm::isa;
using llvm::raw_string_ostream;
using std::function;
using std::vector;

namespace include_what_you_use {

namespace {

void DumpASTNode(llvm::raw_ostream& ostream, const ASTNode* node) {
  if (const Decl *decl = node->GetAs<Decl>()) {
    ostream << "[" << GetKindName(decl) << "] " << PrintableDecl(decl);
  } else if (const Stmt *stmt = node->GetAs<Stmt>()) {
    ostream << "[" << GetKindName(stmt) << "] " << PrintableStmt(stmt);
  } else if (const TypeLoc* typeloc = node->GetAs<TypeLoc>()) {
    ostream << "[" << GetKindName(*typeloc) << "] "
            << PrintableTypeLoc(*typeloc);
  } else if (const Type* type = node->GetAs<Type>()) {  // +typeloc
    ostream << "[" << GetKindName(type) << "] " << PrintableType(type);
  } else if (const NestedNameSpecifier* nns =
                 node->GetAs<NestedNameSpecifier>()) {
    ostream << "[NestedNameSpecifier] " << PrintableNestedNameSpecifier(nns);
  } else if (const TemplateName* tpl_name = node->GetAs<TemplateName>()) {
    ostream << "[TemplateName] " << PrintableTemplateName(*tpl_name);
  } else if (const TemplateArgumentLoc* tpl_argloc =
                 node->GetAs<TemplateArgumentLoc>()) {
    ostream << "[TemplateArgumentLoc] "
            << PrintableTemplateArgumentLoc(*tpl_argloc);
  } else if (const TemplateArgument* tpl_arg =
                 node->GetAs<TemplateArgument>()) {
    ostream << "[TemplateArgument] " << PrintableTemplateArgument(*tpl_arg);
  } else {
    CHECK_UNREACHABLE_("Unknown kind for ASTNode");
  }
}

TemplateSpecializationKind GetTemplateSpecializationKind(const Decl* decl) {
  if (const auto* record = dyn_cast<CXXRecordDecl>(decl)) {
    return record->getTemplateSpecializationKind();
  }
  return clang::TSK_Undeclared;
}

// Desugars to a non-alias template specialization type, if present in the sugar
// chain, or to canonical type.
static const Type* DesugarAliasTypes(const Type* type) {
  type = Desugar(type);

  if (const auto* typedef_type = dyn_cast<TypedefType>(type)) {
    const TypedefNameDecl* decl = typedef_type->getDecl();
    return DesugarAliasTypes(decl->getUnderlyingType().getTypePtr());
  }
  if (const auto* tpl_spec_type = dyn_cast<TemplateSpecializationType>(type)) {
    if (tpl_spec_type->isTypeAlias())
      return DesugarAliasTypes(tpl_spec_type->getAliasedType().getTypePtr());
  }
  return type;
}

static bool IsImplicitConversionCtor(const CXXConstructorDecl* ctor) {
  return !ctor->isExplicit() && ctor->getNumParams() == 1 &&
         !ctor->isCopyOrMoveConstructor();
}

}  // anonymous namespace

//------------------------------------------------------------
// ASTNode and associated utilities.

SourceLocation ASTNode::GetLocation() const {
  SourceLocation retval;
  if (FillLocationIfKnown(&retval))
    return retval;

  // OK, let's ask a parent node.
  for (const ASTNode* node = parent_; node != nullptr; node = node->parent_) {
    if (node->FillLocationIfKnown(&retval))
      break;
  }
  // If the parent node shows the spelling and instantiation
  // locations are in a different file, then we're uncertain of our
  // own location.  Return an invalid location.
  if (retval.isValid()) {
    SourceManager& sm = *GlobalSourceManager();
    OptionalFileEntryRef spelling_file =
        sm.getFileEntryRefForID(sm.getFileID(sm.getSpellingLoc(retval)));
    OptionalFileEntryRef instantiation_file =
        sm.getFileEntryRefForID(sm.getFileID(sm.getExpansionLoc(retval)));
    if (spelling_file != instantiation_file)
      return SourceLocation();
  }

  return retval;
}

bool ASTNode::FillLocationIfKnown(SourceLocation* loc) const {
  using include_what_you_use::GetLocation;
  switch (kind_) {
    case kDeclKind:
      *loc = GetLocation(as_decl_);   // in iwyu_location_util.h
      return true;
    case kStmtKind:
      *loc = GetLocation(as_stmt_);
      return true;
    case kTypelocKind:
      *loc = GetLocation(as_typeloc_);
      return true;
    case kNNSLocKind:
      *loc = GetLocation(as_nnsloc_);
      return true;
    case kTemplateArgumentLocKind:
      *loc = GetLocation(as_template_argloc_);
      return true;
    case kTypeKind:
    case kNNSKind:
    case kTemplateNameKind:
    case kTemplateArgumentKind:
      return false;
  }
  CHECK_UNREACHABLE_("Unexpected kind of ASTNode");
}

// --- Utilities for ASTNode.

bool IsElaboratedTypeSpecifier(const ASTNode* ast_node) {
  if (ast_node == nullptr)
    return false;
  const ElaboratedType* elaborated_type = ast_node->GetAs<ElaboratedType>();
  return elaborated_type &&
         elaborated_type->getKeyword() != ElaboratedTypeKeyword::None;
}

const ASTNode* MostElaboratedAncestor(const ASTNode* ast_node) {
  // Read past elaborations like 'class' keyword or namespaces.
  while (ast_node->ParentIsA<ElaboratedType>()) {
    ast_node = ast_node->parent();
  }
  return ast_node;
}

bool IsQualifiedNameNode(const ASTNode* ast_node) {
  if (ast_node == nullptr)
    return false;
  const ElaboratedType* elaborated_type = ast_node->GetAs<ElaboratedType>();
  if (elaborated_type == nullptr)
    return false;
  return elaborated_type->getQualifier() != nullptr;
}

bool IsNodeInsideCXXMethodBody(const ASTNode* ast_node) {
  // If we're a destructor, we're definitely part of a method body;
  // destructors don't have any other parts to them.  This case is
  // triggered when we see implicit destruction of member vars.
  if (ast_node && ast_node->IsA<CXXDestructorDecl>())
    return true;
  for (; ast_node != nullptr; ast_node = ast_node->parent()) {
    // If we're a constructor, check if we're part of the
    // initializers, which also count as 'the body' of the method.
    if (const CXXConstructorDecl* ctor =
        ast_node->GetParentAs<CXXConstructorDecl>()) {
      for (CXXConstructorDecl::init_const_iterator
               it = ctor->init_begin(); it != ctor->init_end(); ++it) {
        if (ast_node->ContentIs((*it)->getInit()))
          return true;
      }
      // Now fall through to see if we're the body of the constructor.
    }
    if (const CXXMethodDecl* method_decl =
        ast_node->GetParentAs<CXXMethodDecl>()) {
      if (ast_node->ContentIs(method_decl->getBody())) {
        return true;
      }
    }
  }
  return false;
}

UseFlags ComputeUseFlags(const ASTNode* ast_node) {
  UseFlags flags = UF_None;

  if (IsNodeInsideCXXMethodBody(ast_node))
    flags |= UF_InCxxMethodBody;

  return flags;
}

bool IsNestedTagAsWritten(const ASTNode* ast_node) {
  return (ast_node->IsA<TagDecl>() &&
          (ast_node->ParentIsA<CXXRecordDecl>() ||
           // For templated nested-classes, a ClassTemplateDecl is interposed.
           (ast_node->ParentIsA<ClassTemplateDecl>() &&
            ast_node->AncestorIsA<CXXRecordDecl>(2))));
}

bool IsDefaultTemplateTemplateArg(const ASTNode* ast_node) {
  // Is ast_node the 'D' in the following:
  //    template<template <typename A> class T = D> class C { ... }
  // ('D' might be something like 'vector').
  // D is a TemplateName, since it's a template, and its parent
  // is a TemplateArgument, since D is inside a template argument.
  // The only way a template name can be in a template argument
  // is if it's a default parameter.
  return (ast_node->IsA<TemplateName>() &&
          ast_node->ParentIsA<TemplateArgument>());
}

bool IsCXXConstructExprInInitializer(const ASTNode* ast_node) {
  if (!ast_node->IsA<CXXConstructExpr>())
    return false;

  CHECK_(ast_node->parent() && "Constructor should not be a top-level node!");

  // Typically, you can tell an initializer because its parent is a
  // constructor decl.  But sometimes -- I'm not exactly sure when --
  // there can be an ExprWithCleanups in the middle.
  return ((ast_node->ParentIsA<CXXConstructorDecl>()) ||
          (ast_node->ParentIsA<ExprWithCleanups>() &&
           ast_node->AncestorIsA<CXXConstructorDecl>(2)));
}

bool IsCXXConstructExprInNewExpr(const ASTNode* ast_node) {
  if (!ast_node->IsA<CXXConstructExpr>())
    return false;

  CHECK_(ast_node->parent() && "Constructor should not be a top-level node!");
  return ast_node->ParentIsA<CXXNewExpr>();
}

bool IsAutocastExpr(const ASTNode* ast_node) {
  const auto* expr = ast_node->GetAs<CXXConstructExpr>();
  if (!expr)
    return false;
  // Explicitly written CXXTemporaryObjectExpr aren't 'autocast' expressions.
  if (expr->getStmtClass() != Stmt::StmtClass::CXXConstructExprClass)
    return false;
  if (!IsImplicitConversionCtor(expr->getConstructor()))
    return false;
  return ast_node->template HasAncestorOfType<CallExpr>();
}

template<typename T>
NestedNameSpecifier* TryGetQualifier(const ASTNode* ast_node) {
  if (ast_node->IsA<T>())
    return ast_node->GetAs<T>()->getQualifier();
  return nullptr;
}

const NestedNameSpecifier* GetQualifier(const ASTNode* ast_node) {
  const NestedNameSpecifier* nns = nullptr;
  if (ast_node->IsA<TemplateName>()) {
    const TemplateName* tn = ast_node->GetAs<TemplateName>();
    if (const DependentTemplateName* dtn = tn->getAsDependentTemplateName())
      nns = dtn->getQualifier();
    else if (const QualifiedTemplateName* qtn =
                 tn->getAsQualifiedTemplateName())
      nns = qtn->getQualifier();
  }
  if (!nns) nns = TryGetQualifier<ElaboratedType>(ast_node);
  if (!nns) nns = TryGetQualifier<DependentNameType>(ast_node);
  if (!nns && ast_node->IsA<DependentTemplateSpecializationType>()) {
    const auto* dtst = ast_node->GetAs<DependentTemplateSpecializationType>();
    nns = dtst->getDependentTemplateName().getQualifier();
  }
  if (!nns) nns = TryGetQualifier<UsingDirectiveDecl>(ast_node);
  if (!nns) nns = TryGetQualifier<EnumDecl>(ast_node);
  if (!nns) nns = TryGetQualifier<RecordDecl>(ast_node);
  if (!nns) nns = TryGetQualifier<DeclaratorDecl>(ast_node);
  if (!nns) nns = TryGetQualifier<FunctionDecl>(ast_node);
  if (!nns) nns = TryGetQualifier<CXXDependentScopeMemberExpr>(ast_node);
  if (!nns) nns = TryGetQualifier<DeclRefExpr>(ast_node);
  if (!nns) nns = TryGetQualifier<DependentScopeDeclRefExpr>(ast_node);
  if (!nns) nns = TryGetQualifier<MemberExpr>(ast_node);
  return nns;
}

const DeclContext* GetDeclContext(const ASTNode* ast_node) {
  for (; ast_node != nullptr; ast_node = ast_node->parent()) {
    if (ast_node->IsA<Decl>())
      return ast_node->GetAs<Decl>()->getDeclContext();
  }
  return nullptr;
}

//------------------------------------------------------------
// Helper functions for working with raw Clang AST nodes.

// --- Printers.

string PrintableLoc(SourceLocation loc) {
  return NormalizeFilePath(loc.printToString(*GlobalSourceManager()));
}

string PrintableDecl(const Decl* decl, bool terse/*=true*/) {
  if (!decl)
    return "<null decl>";

  // Use the terse flag to limit the level of output to one line.
  PrintingPolicy policy = decl->getASTContext().getPrintingPolicy();
  policy.TerseOutput = terse;
  policy.SuppressInitializers = terse;
  policy.PolishForDeclaration = terse;

  std::string buffer;
  raw_string_ostream ostream(buffer);
  decl->print(ostream, policy);
  return ostream.str();
}

string PrintableStmt(const Stmt* stmt) {
  if (!stmt)
    return "<null stmt>";

  std::string buffer;
  raw_string_ostream ostream(buffer);
  ASTDumper dumper(ostream, /*ShowColors=*/false);
  dumper.Visit(stmt);
  return ostream.str();
}

string PrintableType(const Type* type) {
  if (!type)
    return "<null type>";

  string typestr = QualType(type, 0).getAsString();
  if (GlobalFlags().HasDebugFlag("printtypeclass")) {
    typestr = GetKindName(type) + ":" + typestr;
  }
  return typestr;
}

string PrintableTypeLoc(const TypeLoc& typeloc) {
  return PrintableType(typeloc.getTypePtr());
}

string PrintableNestedNameSpecifier(const NestedNameSpecifier* nns) {
  if (!nns)
    return "<null nns>";

  std::string buffer;  // llvm wants regular string, not our versa-string
  raw_string_ostream ostream(buffer);
  nns->print(ostream, DefaultPrintPolicy());
  return ostream.str();
}

string PrintableTemplateName(const TemplateName& tpl_name) {
  std::string buffer;    // llvm wants regular string, not our versa-string
  raw_string_ostream ostream(buffer);
  tpl_name.print(ostream, DefaultPrintPolicy());
  return ostream.str();
}

string PrintableTemplateArgument(const TemplateArgument& arg) {
  std::string buffer;
  raw_string_ostream ostream(buffer);
  printTemplateArgumentList(ostream, ArrayRef<TemplateArgument>(arg),
                            DefaultPrintPolicy());
  return ostream.str();
}

string PrintableTemplateArgumentLoc(const TemplateArgumentLoc& arg) {
  std::string buffer;
  raw_string_ostream ostream(buffer);
  printTemplateArgumentList(ostream, ArrayRef<TemplateArgumentLoc>(arg),
                            DefaultPrintPolicy());
  return ostream.str();
}

string PrintableASTNode(const ASTNode* node) {
  if (!node)
    return "<null ast node>";

  std::string buffer;
  raw_string_ostream ostream(buffer);
  DumpASTNode(ostream, node);
  return ostream.str();
}

// These print to stderr. They're useful for debugging (e.g. inside gdb).
void PrintASTNode(const ASTNode* node) {
  DumpASTNode(errs(), node);
  errs() << "\n";
}

void PrintStmt(const Stmt* stmt) {
  ASTDumper dumper(llvm::errs(), /*ShowColors=*/false);
  dumper.Visit(stmt);
}

string GetWrittenQualifiedNameAsString(const NamedDecl* named_decl) {
  std::string retval;
  llvm::raw_string_ostream ostream(retval);
  PrintingPolicy printing_policy =
      named_decl->getASTContext().getPrintingPolicy();
  printing_policy.SuppressUnwrittenScope = true;
  named_decl->printQualifiedName(ostream, printing_policy);
  return ostream.str();
}

// --- Utilities for Template Arguments.

// If the TemplateArgument is a type (and not an expression such as
// 'true', or a template such as 'vector', etc), return it.  Otherwise
// return nullptr.
static const Type* GetTemplateArgAsType(const TemplateArgument& tpl_arg) {
  if (tpl_arg.getKind() == TemplateArgument::Type)
    return tpl_arg.getAsType().getTypePtr();
  return nullptr;
}

static bool IsSubstTemplateTypeParmType(const Type* type) {
  const Type* prev_type = type;
  do {
    if (isa<SubstTemplateTypeParmType>(type))
      return true;
    if (isa<TypedefType>(type) || isa<TemplateSpecializationType>(type)) {
      // Still may be substituted outer template parameter but needs additional
      // analysis outside this function.
      return false;
    }
    prev_type = type;
    type = type->getLocallyUnqualifiedSingleStepDesugaredType().getTypePtr();
  } while (type != prev_type);
  return false;
}

// These utilities figure out the template arguments and other type components
// that are specified in various contexts: TemplateSpecializationType (for
// template classes), FunctionDecl (for template functions), and type aliases.
//
// For classes, we care only about explicitly specified template args,
// not implicit, default args.  For functions, we care about all
// template args, since if not specified they're derived from the
// function arguments.  In either case, we only care about template
// arguments that are types (including template types), not other
// kinds of arguments such as built-in types.

// This helper class visits a given AST node and finds all the types
// beneath it, which it returns as a set.  For example, if you have a
// VarDecl 'vector<int(*)(const MyClass&)> x', it would return
// (vector<int(*)(const MyClass&)>, int(*)(const MyClass&),
// int(const MyClass&), int, const MyClass&, MyClass).  Note that
// this function only returns types-as-written, so it does *not* return
// alloc<int(*)(const MyClass&)>, even though it's part of vector.

class SugaredTypeEnumerator
    : public RecursiveASTVisitor<SugaredTypeEnumerator> {
 public:
  // --- Public interface
  // We can add more entry points as needed.
  set<const Type*> Enumerate(const Type* type) {
    seen_types_.clear();
    if (!type)
      return seen_types_;
    TraverseType(QualType(type, 0));
    return seen_types_;
  }

  set<const Type*> Enumerate(const TemplateArgument& tpl_arg) {
    seen_types_.clear();
    TraverseTemplateArgument(tpl_arg);
    return seen_types_;
  }

  // --- Methods on RecursiveASTVisitor
  bool TraverseType(QualType type) {
    if (type.isNull())
      return Base::TraverseType(type);
    return TraverseTypeHelper(type);
  }

  bool TraverseTypeLoc(TypeLoc type_loc) {
    if (!type_loc)
      return Base::TraverseTypeLoc(type_loc);
    return TraverseTypeHelper(type_loc.getType());
  }

 private:
  typedef RecursiveASTVisitor<SugaredTypeEnumerator> Base;

  bool TraverseTypeHelper(QualType qual_type) {
    CHECK_(!qual_type.isNull());

    const Type* desugared_until_alias_or_tpl = Desugar(qual_type.getTypePtr());
    seen_types_.insert(desugared_until_alias_or_tpl);

    const Type* desugared = DesugarAliasTypes(desugared_until_alias_or_tpl);

    // Add desugared type components, if any. Don't add two types with the same
    // canonical type.
    // TODO(bolshakov): it doesn't always work properly. For example, both A and
    // TypedefForA would be inserted in seen_types_ for Tpl<A, TypedefForA>,
    // leading to indeterminate resugar_map construction.
    return Base::TraverseType(QualType(desugared, 0));
  }

  set<const Type*> seen_types_;
};

class TypeEnumeratorWithoutSubstituted
    : public RecursiveASTVisitor<TypeEnumeratorWithoutSubstituted> {
 public:
  // --- Public interface
  set<const Type*> Enumerate(const Type* type) {
    seen_types_.clear();
    if (!type)
      return seen_types_;
    TraverseType(QualType(type, 0));
    return seen_types_;
  }

  // --- Methods on RecursiveASTVisitor
  bool TraverseType(QualType type) {
    if (type.isNull())
      return Base::TraverseType(type);
    return TraverseTypeHelper(type);
  }

  bool TraverseTypeLoc(TypeLoc type_loc) {
    if (!type_loc)
      return Base::TraverseTypeLoc(type_loc);
    return TraverseTypeHelper(type_loc.getType());
  }

 private:
  typedef RecursiveASTVisitor<TypeEnumeratorWithoutSubstituted> Base;

  bool TraverseTypeHelper(QualType qual_type) {
    CHECK_(!qual_type.isNull());
    const Type* type = qual_type.getTypePtr();
    if (IsSubstTemplateTypeParmType(type))
      return true;

    seen_types_.insert(Desugar(type));

    return Base::TraverseType(qual_type);
  }

  set<const Type*> seen_types_;
};

class CanonicalTypeEnumerator
    : public RecursiveASTVisitor<CanonicalTypeEnumerator> {
 public:
  // --- Public interface
  set<const Type*> Enumerate(const Type* type) {
    seen_types_.clear();
    if (!type)
      return seen_types_;
    TraverseType(QualType(type, 0));
    return seen_types_;
  }

  // --- Methods on RecursiveASTVisitor
  bool TraverseType(QualType type) {
    if (type.isNull())
      return Base::TraverseType(type);
    return TraverseTypeHelper(type);
  }

  bool TraverseTypeLoc(TypeLoc type_loc) {
    if (!type_loc)
      return Base::TraverseTypeLoc(type_loc);
    return TraverseTypeHelper(type_loc.getType());
  }

 private:
  typedef RecursiveASTVisitor<CanonicalTypeEnumerator> Base;

  bool TraverseTypeHelper(QualType qual_type) {
    CHECK_(!qual_type.isNull());

    const Type* type = qual_type.getTypePtr();
    seen_types_.insert(GetCanonicalType(type));

    const Type* desugared = DesugarAliasTypes(type);
    // Add desugared type components.
    return Base::TraverseType(QualType(desugared, 0));
  }

  set<const Type*> seen_types_;
};

// A 'component' of a type is a type beneath it in the AST tree.
// So 'Foo*' has component 'Foo', as does 'vector<Foo>', while
// vector<pair<Foo, Bar>> has components pair<Foo,Bar>, Foo, and Bar.
set<const Type*> GetComponentsOfType(const Type* type) {
  SugaredTypeEnumerator type_enumerator;
  return type_enumerator.Enumerate(type);
}

set<const Type*> GetComponentsOfTypeWithoutSubstituted(const Type* type) {
  TypeEnumeratorWithoutSubstituted type_enumerator;
  return type_enumerator.Enumerate(type);
}

set<const Type*> GetCanonicalComponentsOfType(const Type* type) {
  CanonicalTypeEnumerator type_enumerator;
  return type_enumerator.Enumerate(type);
}

// --- Utilities for Decl.

bool IsTemplatizedFunctionDecl(const FunctionDecl* decl) {
  return decl && decl->getTemplateSpecializationArgs() != nullptr;
}

bool IsImplicitInstantiation(const VarDecl* decl) {
  return decl->getTemplateSpecializationKind() ==
         clang::TSK_ImplicitInstantiation;
}

bool HasImplicitConversionCtor(const CXXRecordDecl* cxx_class) {
  // Clang leaves ClassTemplateSpecializationDecl empty for uninstantiated
  // specializations. Hence, replace cxx_class with template definition so that
  // IWYU can find constructors.
  if (const auto* tpl_spec =
          dyn_cast<ClassTemplateSpecializationDecl>(cxx_class)) {
    if (!tpl_spec->isExplicitSpecialization()) {
      cxx_class = tpl_spec->getSpecializedTemplate()
                      ->getCanonicalDecl()
                      ->getTemplatedDecl();
    }
    // TODO(bolshakov): handle partial specializations.
  }

  for (CXXRecordDecl::ctor_iterator ctor = cxx_class->ctor_begin();
       ctor != cxx_class->ctor_end(); ++ctor) {
    if (!IsImplicitConversionCtor(*ctor))
      continue;
    return true;
  }
  return false;
}

// C++ [class.virtual]p8:
//   If the return type of D::f differs from the return type of B::f, the
//   class type in the return type of D::f shall be complete at the point of
//   declaration of D::f or shall be the class type D.
bool HasCovariantReturnType(const CXXMethodDecl* method_decl) {
  QualType derived_return_type = method_decl->getReturnType();

  for (CXXMethodDecl::method_iterator
       it = method_decl->begin_overridden_methods();
       it != method_decl->end_overridden_methods(); ++it) {
    // There are further constraints on covariant return types as such
    // (e.g. parents must be related, derived override must have return type
    // derived from base override, etc.) but the only _valid_ case I can think
    // of where return type differs is when they're actually covariant.
    // That is, if Clang can already compile this code without errors, and
    // return types differ, it can only be due to covariance.
    if ((*it)->getReturnType().getCanonicalType() !=
        derived_return_type.getCanonicalType())
      return true;
  }

  return false;
}

const TagDecl* GetTagDefinition(const Decl* decl) {
  const TagDecl* as_tag = DynCastFrom(decl);
  const ClassTemplateDecl* as_tpl = DynCastFrom(decl);
  if (as_tpl) {  // Convert the template to its underlying class defn.
    as_tag = DynCastFrom(as_tpl->getTemplatedDecl());
  }
  if (as_tag) {
    if (const TagDecl* tag_dfn = as_tag->getDefinition()) {
      return tag_dfn;
    }
    // If we're a templated class that was never instantiated (because
    // we were never "used"), then getDefinition() will return nullptr.
    if (const ClassTemplateSpecializationDecl* spec_decl = DynCastFrom(decl)) {
      PointerUnion<ClassTemplateDecl*,
          ClassTemplatePartialSpecializationDecl*>
          specialized_decl = spec_decl->getSpecializedTemplateOrPartial();
      if (const ClassTemplatePartialSpecializationDecl*
          partial_spec_decl =
          specialized_decl.dyn_cast<
          ClassTemplatePartialSpecializationDecl*>()) {
        // decl would be instantiated from a template partial
        // specialization.
        CHECK_(partial_spec_decl->hasDefinition());
        return partial_spec_decl->getDefinition();
      } else if (const ClassTemplateDecl* tpl_decl =
                 specialized_decl.dyn_cast<ClassTemplateDecl*>()) {
        // decl would be instantiated from a non-specialized
        // template.
        if (tpl_decl->getTemplatedDecl()->hasDefinition())
          return tpl_decl->getTemplatedDecl()->getDefinition();
      }
    }
  }
  return nullptr;
}

SourceRange GetSourceRangeOfClassDecl(const Decl* decl) {
  // If we're a templatized class, go 'up' a level to get the
  // template<...> prefix as well.
  if (const CXXRecordDecl* cxx_decl = DynCastFrom(decl)) {
    if (cxx_decl->getDescribedClassTemplate())
      return cxx_decl->getDescribedClassTemplate()->getSourceRange();
  }
  // We can get source ranges of classes and template classes.
  if (const TagDecl* tag_decl = DynCastFrom(decl))
    return tag_decl->getSourceRange();
  if (const TemplateDecl* tpl_decl = DynCastFrom(decl))
    return tpl_decl->getSourceRange();
  CHECK_UNREACHABLE_("Cannot get source range for this decl type");
}

// Use a local RAV implementation to simply collect all FunctionDecls marked for
// late template parsing. This happens with the flag -fdelayed-template-parsing,
// which is on by default in MSVC-compatible mode.
set<FunctionDecl*> GetLateParsedFunctionDecls(TranslationUnitDecl* decl) {
  struct Visitor : public RecursiveASTVisitor<Visitor> {
    bool VisitFunctionDecl(FunctionDecl* function_decl) {
      if (function_decl->isLateTemplateParsed())
        late_parsed_decls.insert(function_decl);
      return true;
    }

    set<FunctionDecl*> late_parsed_decls;
  };

  Visitor v;
  v.TraverseDecl(decl);

  return v.late_parsed_decls;
}

// Helper for the Get*ResugarMap*() functions.  Given a map from
// desugared->resugared types, looks at each component of the
// resugared type (eg, both hash_set<Foo>* and vector<hash_set<Foo>>
// have two components: hash_set<Foo> and Foo), and returns a map that
// contains the original map elements plus mapping for the components.
// This is because when a type is 'owned' by the template
// instantiator, all parts of the type are owned.  We only consider
// type-components as written.
static map<const Type*, const Type*> ResugarTypeComponents(
    const map<const Type*, const Type*>& resugar_map) {
  map<const Type*, const Type*> retval = resugar_map;
  for (const auto& types : resugar_map) {
    const set<const Type*>& components = GetComponentsOfType(types.second);
    for (const Type* component_type : components) {
      const Type* desugared_type = GetCanonicalType(component_type);
      if (!ContainsKey(retval, desugared_type)) {
        retval[desugared_type] = component_type;
        VERRS(6) << "Adding a type-components of interest: "
                 << PrintableType(component_type) << "\n";
      }
    }
  }
  return retval;
}

// Helpers for GetTplInstDataForFunction().
static map<const Type*, const Type*> GetTplTypeResugarMapForFunctionNoCallExpr(
    const FunctionDecl* decl, unsigned start_arg) {
  map<const Type*, const Type*> retval;
  if (!decl)   // can be nullptr if the function call is via a function pointer
    return retval;
  if (const TemplateArgumentList* tpl_list =
          decl->getTemplateSpecializationArgs()) {
    for (unsigned i = start_arg; i < tpl_list->size(); ++i) {
      if (const Type* arg_type = GetTemplateArgAsType(tpl_list->get(i))) {
        retval[GetCanonicalType(arg_type)] = arg_type;
        VERRS(6) << "Adding an implicit tpl-function type of interest: "
                 << PrintableType(arg_type) << "\n";
      }
    }
  }
  return retval;
}

static map<const Type*, const Type*> GetTplTypeResugarMapForExplicitTplArgs(
    const TemplateArgumentListInfo& explicit_tpl_list) {
  map<const Type*, const Type*> retval;
  for (const TemplateArgumentLoc& loc : explicit_tpl_list.arguments()) {
    if (const Type* arg_type = GetTemplateArgAsType(loc.getArgument())) {
      retval[GetCanonicalType(arg_type)] = arg_type;
      VERRS(6) << "Adding an explicit template argument type of interest: "
               << PrintableType(arg_type) << "\n";
    }
  }
  return retval;
}

// Get the type of an expression while preserving as much type sugar as
// possible. This was originally designed for use with function argument
// expressions, and so might not work in a more general context.
static const Type* GetSugaredTypeOf(const Expr* expr) {
  // Search the expression subtree for better sugar; stop as soon as a type
  // different from expr's type is found.
  struct Visitor : public RecursiveASTVisitor<Visitor> {
    Visitor(QualType origtype) : sugared(origtype.getLocalUnqualifiedType()) {
    }

    bool VisitDeclRefExpr(DeclRefExpr* e) {
      return !CollectSugar(e);
    }

    bool VisitImplicitCastExpr(ImplicitCastExpr* e) {
      return !CollectSugar(e->getSubExpr());
    }

    bool CollectSugar(const Expr* e) {
      QualType exprtype = e->getType().getLocalUnqualifiedType();
      if (!exprtype.isNull() && exprtype != sugared) {
        sugared = exprtype;
        return true;
      }

      return false;
    }

    QualType sugared;
  };

  // Default to the expr's type.
  Visitor v(expr->getType());
  v.TraverseStmt(const_cast<Expr*>(expr));

  return v.sugared.getTypePtr();
}

static map<const Type*, const Type*> GetDefaultedArgResugarMap(
    const FunctionDecl* decl) {
  map<const Type*, const Type*> res;
  const FunctionTemplateDecl* template_decl = decl->getPrimaryTemplate();
  if (!template_decl)
    return res;
  const TemplateParameterList* params = template_decl->getTemplateParameters();
  const TemplateArgumentList* args = decl->getTemplateSpecializationArgs();
  const unsigned count = params->size();
  CHECK_(args->size() == count);
  for (unsigned i = 0; i < count; ++i) {
    if (const auto* param_decl =
            dyn_cast<TemplateTypeParmDecl>(params->getParam(i))) {
      if (param_decl->isParameterPack())
        continue;
      if (!param_decl->hasDefaultArgument())
        continue;
      const QualType arg_type = args->get(i).getAsType().getCanonicalType();
      const QualType default_arg_type = param_decl->getDefaultArgument()
                                            .getArgument()
                                            .getAsType()
                                            .getCanonicalType();
      if (arg_type == default_arg_type) {
        res.emplace(arg_type.getTypePtr(), nullptr);
      }
    }
  }
  return res;
}

static map<const Type*, const Type*> GetDefaultedArgResugarMap(
    const VarTemplateSpecializationDecl* decl, const DeclRefExpr* expr) {
  map<const Type*, const Type*> res;
  unsigned int num_explicit_args = expr->getNumTemplateArgs();
  const TemplateArgumentList& args = decl->getTemplateArgs();
  for (unsigned i = num_explicit_args, ie = args.size(); i < ie; ++i) {
    const TemplateArgument& arg = args[i];
    if (arg.getKind() == TemplateArgument::ArgKind::Type)
      res.emplace(arg.getAsType().getTypePtr(), nullptr);
  }
  return res;
}

TemplateInstantiationData GetTplInstDataForFunction(
    const FunctionDecl* decl, const Expr* calling_expr,
    function<set<const Type*>(const Type*)> provided_getter) {
  map<const Type*, const Type*> resugar_map;
  set<const Type*> provided_types;

  // If calling_expr is nullptr, then we can't find any explicit template
  // arguments, if they were specified (e.g. 'Fn<int>()'), and we
  // won't be able to get the function arguments as written.  So we
  // can't resugar at all.  We just have to hope that the types happen
  // to be already sugared, because the actual-type is already canonical.
  if (calling_expr == nullptr) {
    resugar_map = GetTplTypeResugarMapForFunctionNoCallExpr(decl, 0);
    resugar_map = ResugarTypeComponents(
        resugar_map);  // add in resugar_map's decomposition
    return TemplateInstantiationData{resugar_map, {}};
  }

  // If calling_expr is a CXXConstructExpr of CXXNewExpr, then it's
  // impossible to explicitly specify template arguments; all we have
  // to go on is function arguments.  If it's a CallExpr, and some
  // arguments might be explicit, and others implicit.  Otherwise,
  // it's a type that doesn't take function template args at all (like
  // CXXDeleteExpr) or only takes explicit args (like DeclRefExpr).
  const Expr* const* fn_args = nullptr;
  unsigned num_args = 0;
  unsigned start_of_implicit_args = 0;
  if (const CXXConstructExpr* ctor_expr = DynCastFrom(calling_expr)) {
    fn_args = ctor_expr->getArgs();
    num_args = ctor_expr->getNumArgs();
  } else if (const CallExpr* call_expr = DynCastFrom(calling_expr)) {
    fn_args = call_expr->getArgs();
    num_args = call_expr->getNumArgs();
    const Expr* callee_expr = call_expr->getCallee()->IgnoreParenCasts();
    const TemplateArgumentListInfo& explicit_tpl_args =
        GetExplicitTplArgs(callee_expr);
    if (explicit_tpl_args.size() > 0) {
      resugar_map = GetTplTypeResugarMapForExplicitTplArgs(explicit_tpl_args);
      start_of_implicit_args = explicit_tpl_args.size();
    }
  } else {
    // If calling_expr has explicit template args, then consider them.
    const TemplateArgumentListInfo& explicit_tpl_args =
        GetExplicitTplArgs(calling_expr);
    if (explicit_tpl_args.size() > 0) {
      resugar_map = GetTplTypeResugarMapForExplicitTplArgs(explicit_tpl_args);
      resugar_map = ResugarTypeComponents(resugar_map);
      for (const auto [_, sugared_type] : resugar_map)
        InsertAllInto(provided_getter(sugared_type), &provided_types);
    }
    InsertAllInto(GetDefaultedArgResugarMap(decl), &resugar_map);
    return TemplateInstantiationData{resugar_map, provided_types};
  }

  // Now we have to figure out, as best we can, the sugar-mappings for
  // compiler-deduced template args.  We do this by looking at every
  // type specified in any part of the function arguments as written.
  // If any of these types matches a template type, then we take that
  // to be the resugar mapping.  If none of the types match, then we
  // assume that the template is matching some desugared part of the
  // type, and we ignore it.  For instance:
  //     operator<<(basic_ostream<char, T>& o, int i);
  // If I pass in an ostream as the first argument, then no part
  // of the (sugared) argument types match T, so we ignore it.
  const map<const Type*, const Type*>& desugared_types =
      GetTplTypeResugarMapForFunctionNoCallExpr(decl, start_of_implicit_args);

  // TODO(csilvers): SubstTemplateTypeParms are always desugared,
  //                 making this less useful than it should be.
  // TODO(csilvers): if the GetArg(i) expr has an implicit cast
  //                 under it, take the pre-cast type instead?
  set<const Type*> fn_arg_types;
  for (unsigned i = 0; i < num_args; ++i) {
    if (isa<CXXDefaultArgExpr>(fn_args[i]))
      break;
    const Type* argtype = GetSugaredTypeOf(fn_args[i]);
    // TODO(csilvers): handle RecordTypes that are a TemplateSpecializationDecl
    InsertAllInto(GetComponentsOfType(argtype), &fn_arg_types);
  }

  for (const Type* type : fn_arg_types) {
    // See if any of the template args in resugar_map are the desugared form
    // of us.
    const Type* desugared_type = GetCanonicalType(type);
    if (ContainsKey(desugared_types, desugared_type)) {
      resugar_map[desugared_type] = type;
      if (desugared_type != type) {
        VERRS(6) << "Remapping template arg of interest: "
                 << PrintableType(desugared_type) << " -> "
                 << PrintableType(type) << "\n";
      }
    }
    // TODO(bolshakov): more fine-grained determination of provided types could
    // e.g. consider only arguments that actually take part in type deduction.
    // Simply moving the line below into the if-statement body above doesn't
    // work because 'type' can be just an internal component of a 'typedef'ed
    // argument.
    InsertAllInto(provided_getter(type), &provided_types);
  }

  InsertAllInto(GetDefaultedArgResugarMap(decl), &resugar_map);

  // Log the types we never mapped.
  for (const auto& types : desugared_types) {
    if (!ContainsKey(resugar_map, types.first)) {
      VERRS(6) << "Ignoring unseen-in-fn-args template arg of interest: "
               << PrintableType(types.first) << "\n";
    }
  }

  resugar_map = ResugarTypeComponents(
      resugar_map);  // add in the decomposition of resugar_map
  for (const auto [_, sugared_type] : resugar_map)
    InsertAllInto(provided_getter(sugared_type), &provided_types);
  return TemplateInstantiationData{resugar_map, provided_types};
}

TemplateInstantiationData GetTplInstDataForVariable(
    const VarTemplateSpecializationDecl* decl,
    const DeclRefExpr* expr,
    function<set<const Type*>(const Type*)> provided_getter) {
  TemplateArgumentListInfo explicit_tpl_args;
  expr->copyTemplateArgumentsInto(explicit_tpl_args);
  map<const Type*, const Type*> resugar_map =
      GetTplTypeResugarMapForExplicitTplArgs(explicit_tpl_args);
  resugar_map = ResugarTypeComponents(resugar_map);
  InsertAllInto(GetDefaultedArgResugarMap(decl, expr), &resugar_map);

  set<const Type*> provided_types;
  for (const auto [_, sugared_type] : resugar_map)
    InsertAllInto(provided_getter(sugared_type), &provided_types);

  return TemplateInstantiationData{resugar_map, provided_types};
}

const NamedDecl* GetInstantiatedFromDecl(const NamedDecl* class_decl) {
  if (const ClassTemplateSpecializationDecl* tpl_sp_decl =
      DynCastFrom(class_decl)) {  // an instantiated class template
    PointerUnion<ClassTemplateDecl*, ClassTemplatePartialSpecializationDecl*>
        instantiated_from = tpl_sp_decl->getInstantiatedFrom();
    if (const ClassTemplateDecl* tpl_decl =
        instantiated_from.dyn_cast<ClassTemplateDecl*>()) {
      // class_decl is instantiated from a non-specialized template.
      return tpl_decl;
    } else if (const ClassTemplatePartialSpecializationDecl*
               partial_spec_decl =
               instantiated_from.dyn_cast<
                   ClassTemplatePartialSpecializationDecl*>()) {
      // class_decl is instantiated from a template partial specialization.
      return partial_spec_decl;
    }
  }
  // class_decl is not instantiated from a template.
  return class_decl;
}

const NamedDecl* GetDefinitionAsWritten(const NamedDecl* decl) {
  // First, get to decl-as-written.
  if (const CXXRecordDecl* class_decl = DynCastFrom(decl)) {
    decl = GetInstantiatedFromDecl(class_decl);
    if (const ClassTemplateDecl* tpl_decl = DynCastFrom(decl))
      decl = tpl_decl->getTemplatedDecl();  // convert back to CXXRecordDecl
  } else if (const FunctionDecl* func_decl = DynCastFrom(decl)) {
    // If we're instantiated from a template, use the template pattern as the
    // decl-as-written.
    // But avoid friend declarations in templates, something happened in Clang
    // r283207 that caused them to form a dedicated redecl chain, separate
    // from all other redecls.
    const FunctionDecl* tp_decl = func_decl->getTemplateInstantiationPattern();
    if (tp_decl && tp_decl->getFriendObjectKind() == Decl::FOK_None)
      decl = tp_decl;
  }
  // Then, get to definition.
  if (const NamedDecl* class_dfn = GetTagDefinition(decl)) {
    return class_dfn;
  } else if (const FunctionDecl* fn_decl = DynCastFrom(decl)) {
    for (FunctionDecl::redecl_iterator it = fn_decl->redecls_begin();
         it != fn_decl->redecls_end(); ++it) {
      if ((*it)->isThisDeclarationADefinition())
        return *it;
    }
  }
  // Couldn't find a definition, just return the original declaration.
  return decl;
}

bool IsFriendDecl(const Decl* decl) {
  // For 'template<...> friend class T', the decl will just be 'class T'.
  // We need to go 'up' a level to check friendship in the right place.
  if (const CXXRecordDecl* cxx_decl = DynCastFrom(decl))
    if (cxx_decl->getDescribedClassTemplate())
      decl = cxx_decl->getDescribedClassTemplate();
  return decl->getFriendObjectKind() != Decl::FOK_None;
}

bool IsExplicitInstantiation(const Decl* decl) {
  TemplateSpecializationKind kind = GetTemplateSpecializationKind(decl);
  return kind == clang::TSK_ExplicitInstantiationDeclaration ||
         kind == clang::TSK_ExplicitInstantiationDefinition;
}

bool IsExplicitInstantiationDefinitionAsWritten(
    const ClassTemplateSpecializationDecl* decl) {
  // When swithing instantiation declaration to definition, clang preserves
  // the 'extern' keyword location info.
  return decl->getSpecializationKind() ==
             clang::TSK_ExplicitInstantiationDefinition &&
         decl->getExternKeywordLoc().isInvalid();
}

bool IsInInlineNamespace(const Decl* decl) {
  const DeclContext* dc = decl->getDeclContext();
  for (; dc; dc = dc->getParent()) {
    if (dc->isInlineNamespace())
      return true;
  }

  return false;
}

bool IsInNamespace(const NamedDecl* decl, const NamespaceDecl* ns_decl) {
  const DeclContext* primary_ns_context = ns_decl->getPrimaryContext();
  for (const DeclContext* dc = decl->getDeclContext(); dc;
       dc = dc->getParent()) {
    if (dc->getPrimaryContext() == primary_ns_context)
      return true;
  }

  return false;
}

bool IsForwardDecl(const NamedDecl* decl) {
  if (const auto* tag_decl = dyn_cast<TagDecl>(decl)) {
    // clang-format off
    return (!tag_decl->getName().empty() &&
            !tag_decl->isCompleteDefinition() &&
            !tag_decl->isEmbeddedInDeclarator() &&
            !IsFriendDecl(tag_decl) &&
            !IsExplicitInstantiation(tag_decl));
  }  // clang-format on

  return false;
}

// Two possibilities: it's written as a nested class (that is, with a
// qualifier) or it's actually living inside another class.
bool IsNestedClass(const TagDecl* decl) {
  if (decl->getQualifier() &&
      decl->getQualifier()->getKind() == NestedNameSpecifier::TypeSpec) {
    return true;
  }
  return isa<RecordDecl>(decl->getDeclContext());
}

bool HasDefaultTemplateParameters(const TemplateDecl* decl) {
  TemplateParameterList* tpl_params = decl->getTemplateParameters();
  return tpl_params->getMinRequiredArguments() < tpl_params->size();
}

template <class T>
inline set<const NamedDecl*> GetRedeclsOfRedeclarable(
    const Redeclarable<T>* decl) {
  return set<const NamedDecl*>(decl->redecls_begin(), decl->redecls_end());
}

// The only way to find out whether a decl can be dyn_cast to a
// Redeclarable<T> and what T is is to enumerate the possibilities.
// Hence we hard-code the list.
set<const NamedDecl*> GetNonTagRedecls(const NamedDecl* decl) {
  CHECK_(!isa<TagDecl>(decl) && "For tag types, call GetTagRedecls()");
  CHECK_(!isa<ClassTemplateDecl>(decl) && "For tpls, call GetTagRedecls()");
  // TODO(wan): go through iwyu to replace TypedefDecl with
  // TypedefNameDecl as needed.
  if (const TypedefNameDecl* specific_decl = DynCastFrom(decl))
    return GetRedeclsOfRedeclarable(specific_decl);
  if (const FunctionDecl* specific_decl = DynCastFrom(decl))
    return GetRedeclsOfRedeclarable(specific_decl);
  if (const VarDecl* specific_decl = DynCastFrom(decl))
    return GetRedeclsOfRedeclarable(specific_decl);
  // Not redeclarable, so the output is just the input.
  set<const NamedDecl*> retval;
  retval.insert(decl);
  return retval;
}

set<const NamedDecl*> GetTagRedecls(const NamedDecl* decl) {
  const TagDecl* tag_decl = DynCastFrom(decl);
  const ClassTemplateDecl* tpl_decl = DynCastFrom(decl);
  if (tpl_decl)
    tag_decl = tpl_decl->getTemplatedDecl();
  if (!tag_decl)
    return set<const NamedDecl*>();

  set<const NamedDecl*> redecls;
  for (TagDecl::redecl_iterator it = tag_decl->redecls_begin();
       it != tag_decl->redecls_end(); ++it) {
    const auto* redecl = cast<TagDecl>(*it);
    // If this decl is a friend decl, don't count it: friend decls
    // don't serve as forward-declarations.  (This should never
    // happen, I think, but it sometimes does due to a clang bug:
    // http://llvm.org/bugs/show_bug.cgi?id=8669).  The only exception
    // is made because every decl is a redecl of itself.
    if (IsFriendDecl(redecl) && redecl != decl)
      continue;

    if (tpl_decl) {   // need to convert back to a ClassTemplateDecl
      CHECK_(isa<CXXRecordDecl>(redecl) &&
             cast<CXXRecordDecl>(redecl)->getDescribedClassTemplate());
      const CXXRecordDecl* cxx_redecl = cast<CXXRecordDecl>(redecl);
      redecls.insert(cxx_redecl->getDescribedClassTemplate());
    } else {
      redecls.insert(redecl);
    }
  }
  return redecls;
}

const NamedDecl* GetFirstRedecl(const NamedDecl* decl) {
  const NamedDecl* first_decl = decl;
  SourceLocation first_decl_loc = GetLocation(first_decl);

  set<const NamedDecl*> all_redecls = GetTagRedecls(decl);
  if (all_redecls.empty())  // input is not a class or class template
    return nullptr;

  SourceManager& sm = *GlobalSourceManager();
  for (const NamedDecl* redecl : all_redecls) {
    SourceLocation redecl_loc = GetLocation(redecl);
    if (sm.isBeforeInTranslationUnit(redecl_loc, first_decl_loc)) {
      first_decl = redecl;
      first_decl_loc = redecl_loc;
    }
  }
  return first_decl;
}

const NamedDecl* GetNonfriendClassRedecl(const NamedDecl* decl) {
  const RecordDecl* record_decl = DynCastFrom(decl);
  const ClassTemplateDecl* tpl_decl = DynCastFrom(decl);
  if (tpl_decl)
    record_decl = tpl_decl->getTemplatedDecl();
  // This check is so we return the input decl whenever possible.
  if (!record_decl || !IsFriendDecl(record_decl))
    return decl;

  set<const NamedDecl*> all_redecls = GetTagRedecls(decl);
  CHECK_(!all_redecls.empty() && "Uncaught non-class decl");
  return *all_redecls.begin();    // arbitrary choice
}

bool DeclsAreInSameClass(const Decl* decl1, const Decl* decl2) {
  if (!decl1 || !decl2)
    return false;
  if (decl1->getDeclContext() != decl2->getDeclContext())
    return false;
  return decl1->getDeclContext()->isRecord();
}

bool IsBuiltinFunction(const NamedDecl* decl) {
  if (const IdentifierInfo* iden = decl->getIdentifier()) {
    unsigned builtin_id = iden->getBuiltinID();
    if (builtin_id != 0) {
      const clang::Builtin::Context& ctx = decl->getASTContext().BuiltinInfo;
      return !ctx.isPredefinedLibFunction(builtin_id) &&
             !ctx.isHeaderDependentFunction(builtin_id);
    }
  }
  return false;
}

bool IsImplicitlyInstantiatedDfn(const FunctionDecl* decl) {
  const FunctionTemplateSpecializationInfo* tpl_spec_info =
      decl->getTemplateSpecializationInfo();
  if (!tpl_spec_info)
    return false;  // Not a template specialization.
  return decl->isThisDeclarationADefinition() &&
         tpl_spec_info->getTemplateSpecializationKind() ==
             clang::TSK_ImplicitInstantiation;
}

const CXXMethodDecl* GetFromLeastDerived(const CXXMethodDecl* decl) {
  while (decl->size_overridden_methods())
    decl = *decl->begin_overridden_methods();
  return decl;
}

// --- Utilities for Type.

const Type* GetTypeOf(const Expr* expr) {
  if (const auto* decl_ref_expr = dyn_cast<DeclRefExpr>(expr))
    return decl_ref_expr->getDecl()->getType().getTypePtr();
  if (const auto* cast_expr = dyn_cast<ExplicitCastExpr>(expr))
    return cast_expr->getTypeAsWritten().getTypePtr();
  return expr->getType().getTypePtr();
}

const Type* GetTypeOf(const CXXConstructExpr* expr) {
  const Type* type = expr->getType().getTypePtr();
  if (const ArrayType* array_type = type->getAsArrayTypeUnsafe()) {
    type = array_type->getElementType().getTypePtr();
  }
  return type;
}

const Type* GetTypeOf(const ValueDecl* decl) {
  return decl->getType().getTypePtr();
}

const Type* GetTypeOf(const TypeDecl* decl) {
  return decl->getTypeForDecl();
}

const Type* GetCanonicalType(const Type* type) {
  if (!type)
    return type;
  QualType canonical_type = type->getCanonicalTypeUnqualified();
  return canonical_type.getTypePtr();
}

// Based on Type::getUnqualifiedDesugaredType.
const Type* Desugar(const Type* type) {
  // Null types happen sometimes in IWYU.
  if (!type) {
    return type;
  }

  const Type* cur = type;

  while (true) {
    // Don't desugar types that (potentially) add a name.
    if (cur->getTypeClass() == Type::Typedef ||
        cur->getTypeClass() == Type::TemplateSpecialization) {
      return cur;
    }

    switch (cur->getTypeClass()) {
#define ABSTRACT_TYPE(Class, Parent)
#define TYPE(Class, Parent)                              \
  case Type::Class: {                                    \
    const auto* derived = cast<clang::Class##Type>(cur); \
    if (!derived->isSugared()) {                         \
      return cur;                                        \
    }                                                    \
    cur = derived->desugar().getTypePtr();               \
    break;                                               \
  }
#include "clang/AST/TypeNodes.inc"
    }
  }
}

bool IsTemplatizedType(const Type* type) {
  return type && type->getAs<TemplateSpecializationType>();
}

bool InvolvesTypeForWhich(const Type* type, function<bool(const Type*)> pred) {
  type = Desugar(type);
  if (pred(type))
    return true;
  const Decl* decl = TypeToDeclAsWritten(type);
  if (const auto* cts_decl =
          dyn_cast_or_null<ClassTemplateSpecializationDecl>(decl)) {
    const TemplateArgumentList& tpl_args = cts_decl->getTemplateArgs();
    for (const TemplateArgument& tpl_arg : tpl_args.asArray()) {
      if (const Type* arg_type = GetTemplateArgAsType(tpl_arg)) {
        if (InvolvesTypeForWhich(arg_type, pred)) {
          return true;
        }
      }
    }
  }
  return false;
}

bool IsPointerOrReferenceAsWritten(const Type* type) {
  type = Desugar(type);
  return isa<PointerType>(type) || isa<ReferenceType>(type);
}

const Type* RemovePointersAndReferencesAsWritten(const Type* type) {
  type = Desugar(type);
  while (isa<PointerType>(type) ||
         isa<LValueReferenceType>(type)) {
    type = type->getPointeeType().getTypePtr();
  }
  return type;
}

const Type* RemovePointerFromType(const Type* type) {
  if (!IsPointerOrReferenceAsWritten(type)) {   // ah well, have to desugar
    type = type->getUnqualifiedDesugaredType();
  }
  if (!IsPointerOrReferenceAsWritten(type)) {
    return nullptr;
  }
  type = Desugar(type);
  type = type->getPointeeType().getTypePtr();
  return type;
}

// This follows typedefs/etc to remove pointers, if necessary.
const Type* RemovePointersAndReferences(const Type* type) {
  while (true) {
    const Type* deref_type = RemovePointerFromType(type);
    if (deref_type == nullptr) // type wasn't a pointer (or reference) type
      break;                   // removed all pointers
    type = deref_type;
  }
  return type;
}

static const NamedDecl* TypeToDeclImpl(const Type* type, bool as_written) {
  // Read past SubstTemplateTypeParmType (this can happen if a function
  // template returns the tpl-arg type: e.g. for 'T MyFn<T>() {...}; MyFn<X>.a',
  // the type of MyFn<X> will be a substitution) as well as any elaboration.
  type = Desugar(type);

  CHECK_(!isa<ObjCObjectType>(type) && "IWYU doesn't support Objective-C");

  // We have to be a bit careful about the order, because we want
  // to keep typedefs as typedefs, so we do the record check last.
  // We use getAs<> when we can -- unfortunately, it only exists
  // for a few types so far.
  const TemplateSpecializationType* template_spec_type = DynCastFrom(type);
  const TemplateDecl* template_decl =
      template_spec_type
          ? template_spec_type->getTemplateName().getAsTemplateDecl()
          : nullptr;

  if (const TypedefType* typedef_type = DynCastFrom(type)) {
    return typedef_type->getDecl();
  } else if (const InjectedClassNameType* icn_type =
                 type->getAs<InjectedClassNameType>()) {
    return icn_type->getDecl();
  } else if (as_written && template_decl &&
             isa<TypeAliasTemplateDecl>(template_decl)) {
    // A template type alias
    return template_decl;
  } else if (const RecordType* record_type = type->getAs<RecordType>()) {
    return record_type->getDecl();
  } else if (const TagType* tag_type = DynCastFrom(type)) {
    return tag_type->getDecl();    // probably just enums
  } else if (template_decl) {
    // A non-concrete template class, such as 'Myclass<T>'
    return template_decl;
  } else if (const FunctionType* function_type = DynCastFrom(type)) {
    // TODO(csilvers): is it possible to map from fn type to fn decl?
    (void)function_type;
    return nullptr;
  } else {
    return nullptr;
  }
}

const NamedDecl* TypeToDeclAsWritten(const Type* type) {
  return TypeToDeclImpl(type, /*as_written=*/true);
}

const NamedDecl* TypeToDeclForContent(const Type* type) {
  return TypeToDeclImpl(type, /*as_written=*/false);
}

QualType RemoveReference(QualType type) {
  if (const auto* ref_type = type->getAs<ReferenceType>())
    return ref_type->getPointeeType();
  return type;
}

const Type* RemoveReferenceAsWritten(const Type* type) {
  if (const LValueReferenceType* ref_type = DynCastFrom(type))
    return ref_type->getPointeeType().getTypePtr();
  else
    return type;
}

bool HasImplicitConversionConstructor(const Type* type) {
  type = Desugar(type);
  if (isa<PointerType>(type))
    return false;  // can't implicitly convert to a pointer
  if (isa<LValueReferenceType>(type) &&
      !type->getPointeeType().isConstQualified())
    return false;  // can't implicitly convert to a non-const reference

  type = RemoveReferenceAsWritten(type);
  const NamedDecl* decl = TypeToDeclAsWritten(type);
  if (!decl)  // not the kind of type that has a decl (e.g. built-in)
    return false;

  const CXXRecordDecl* cxx_class = DynCastFrom(decl);
  if (!cxx_class)
    return false;  // can't implicitly convert to a non-class type

  return HasImplicitConversionCtor(cxx_class);
}

static TemplateInstantiationData GetTplInstDataForClassNoComponentTypes(
    ArrayRef<TemplateArgument> written_tpl_args,
    const ClassTemplateSpecializationDecl* cls_tpl_decl,
    function<set<const Type*>(const Type*)> provided_getter) {
  map<const Type*, const Type*> retval;

  // Pull the template arguments out of the specialization type. If this is
  // a ClassTemplateSpecializationDecl specifically, we want to
  // get the arguments therefrom to correctly handle default arguments.
  llvm::ArrayRef<TemplateArgument> tpl_args = written_tpl_args;
  unsigned num_args = tpl_args.size();

  if (cls_tpl_decl) {
    const TemplateArgumentList& tpl_arg_list =
        cls_tpl_decl->getTemplateInstantiationArgs();
    tpl_args = tpl_arg_list.asArray();
    num_args = tpl_arg_list.size();
  }

  // TemplateSpecializationType only includes explicitly specified
  // types in its args list, so we start with that.  Note that an
  // explicitly specified type may fulfill multiple template args:
  //   template <typename R, typename A1> struct Foo<R(A1)> { ... }
  set<unsigned> explicit_args;   // indices into tpl_args we've filled
  SugaredTypeEnumerator type_enumerator;
  set<const Type*> provided_types;
  for (unsigned i = 0; i < written_tpl_args.size(); ++i) {
    set<const Type*> arg_components =
        type_enumerator.Enumerate(written_tpl_args[i]);
    // Go through all template types mentioned in the arg-as-written,
    // and compare it against each of the types in the template decl
    // (the latter are all desugared).  If there's a match, update
    // the mapping.
    for (const Type* type : arg_components) {
      for (unsigned i = 0; i < num_args; ++i) {
        if (const Type* arg_type = GetTemplateArgAsType(tpl_args[i])) {
          if (GetCanonicalType(type) == GetCanonicalType(arg_type)) {
            retval[arg_type] = type;
            VERRS(6) << "Adding a template-class type of interest: "
                     << PrintableType(arg_type) << " -> " << PrintableType(type)
                     << "\n";
            explicit_args.insert(i);
          }
        }
      }
    }

    for (const Type* component : arg_components)
      InsertAllInto(provided_getter(component), &provided_types);
  }

  // Now take a look at the args that were not filled explicitly.
  for (unsigned i = 0; i < num_args; ++i) {
    if (ContainsKey(explicit_args, i))
      continue;
    if (const Type* arg_type = GetTemplateArgAsType(tpl_args[i])) {
      retval[arg_type] = nullptr;
      VERRS(6) << "Adding a template-class default type of interest: "
               << PrintableType(arg_type) << "\n";
    }
  }

  return TemplateInstantiationData{retval, provided_types};
}

TemplateInstantiationData GetTplInstDataForClassNoComponentTypes(
    const clang::Type* type,
    std::function<set<const clang::Type*>(const clang::Type*)>
        provided_getter) {
  const auto* tpl_spec_type = type->getAs<TemplateSpecializationType>();
  if (!tpl_spec_type)
    return TemplateInstantiationData{};
  const NamedDecl* decl = TypeToDeclAsWritten(tpl_spec_type);
  const auto* cls_tpl_decl = dyn_cast<ClassTemplateSpecializationDecl>(decl);
  return GetTplInstDataForClassNoComponentTypes(
      tpl_spec_type->template_arguments(), cls_tpl_decl, provided_getter);
}

TemplateInstantiationData GetTplInstDataForClass(
    const Type* type, function<set<const Type*>(const Type*)> provided_getter) {
  TemplateInstantiationData result =
      GetTplInstDataForClassNoComponentTypes(type, provided_getter);
  InsertAllInto(provided_getter(type), &result.provided_types);
  return TemplateInstantiationData{
      ResugarTypeComponents(
          result.resugar_map),  // add in the decomposition of retval
      result.provided_types};
}

TemplateInstantiationData GetTplInstDataForClass(
    ArrayRef<TemplateArgument> written_tpl_args,
    const ClassTemplateSpecializationDecl* cls_tpl_decl,
    function<set<const Type*>(const Type*)> provided_getter) {
  TemplateInstantiationData result = GetTplInstDataForClassNoComponentTypes(
      written_tpl_args, cls_tpl_decl, provided_getter);
  return TemplateInstantiationData{
      ResugarTypeComponents(
          result.resugar_map),  // add in the decomposition of retval
      result.provided_types};
}

bool CanBeOpaqueDeclared(const EnumType* type) {
  return type->getDecl()->isFixed();
}

vector<const Type*> GetCanonicalArgComponents(
    const TemplateSpecializationType* type) {
  vector<const Type*> res;
  SugaredTypeEnumerator enumerator;
  for (const TemplateArgument& arg : type->template_arguments()) {
    for (const Type* component : enumerator.Enumerate(arg))
      res.push_back(GetCanonicalType(component));
  }
  return res;
}

bool IsReferenceToModifiableLValue(const Type* type) {
  return type->isLValueReferenceType() &&
         !type->getPointeeType().getQualifiers().hasConst();
}

bool RefCanBindToTemp(const Type* type) {
  if (type->isRValueReferenceType())
    return true;
  const auto* ref_type = type->getAs<LValueReferenceType>();
  if (!ref_type)
    return false;
  const QualType referred_type = ref_type->getPointeeType();
  return referred_type.isConstQualified() &&
         !referred_type.isVolatileQualified();
}

bool IsDerivedToBasePtrConvertible(QualType derived_ptr_type,
                                   QualType base_ptr_type) {
  if (!base_ptr_type->isPointerType())
    return false;
  const QualType base = base_ptr_type->getPointeeType();
  const CXXRecordDecl* base_decl = base->getAsCXXRecordDecl();
  if (!base_decl)
    return false;
  const ASTContext& context = base_decl->getASTContext();

  QualType derived;
  if (derived_ptr_type->isPointerType()) {
    derived = derived_ptr_type->getPointeeType();
  } else if (const ArrayType* array =
                 context.getAsArrayType(derived_ptr_type)) {
    derived = array->getElementType();
  } else {
    return false;
  }

  if (const CXXRecordDecl* derived_decl = derived->getAsCXXRecordDecl()) {
    if (!derived_decl->isDerivedFrom(base_decl))
      return false;
    return base.isAtLeastAsQualifiedAs(derived, context);
  }
  return false;
}

bool IsBaseToDerivedMemPtrConvertible(const Type* maybe_base_mem_ptr_type,
                                      const Type* maybe_derived_mem_ptr_type,
                                      Sema& sema) {
  const auto* base_mem_ptr_type =
      maybe_base_mem_ptr_type->getAs<MemberPointerType>();
  const auto* derived_mem_ptr_type =
      maybe_derived_mem_ptr_type->getAs<MemberPointerType>();
  if (!base_mem_ptr_type || !derived_mem_ptr_type)
    return false;
  const CXXRecordDecl* base_decl =
      base_mem_ptr_type->getQualifier()->getAsRecordDecl();
  const CXXRecordDecl* derived_decl =
      derived_mem_ptr_type->getQualifier()->getAsRecordDecl();
  if (!derived_decl->isDerivedFrom(base_decl))
    return false;
  const QualType base_mem_type =
      base_mem_ptr_type->getPointeeType().getCanonicalType();
  const QualType derived_mem_type =
      derived_mem_ptr_type->getPointeeType().getCanonicalType();
  // The unqualified canonical member types should be the same except the known
  // acceptable differences in 'noexcept' specifier of member functions.
  if (base_mem_type->isFunctionProtoType()) {
    if (sema.IsFunctionConversion(base_mem_type, derived_mem_type))
      return true;
  }
  if (base_mem_type.getTypePtr() != derived_mem_type.getTypePtr())
    return false;
  return derived_mem_type.isAtLeastAsQualifiedAs(base_mem_type,
                                                 base_decl->getASTContext());
}

bool IsConvertible(QualType from,
                   QualType to,
                   SourceLocation conv_loc,
                   Sema& sema) {
  // Taken from clang/lib/Sema/SemaExprCXX.cpp (see e.g.
  // CheckConvertibilityForTypeTraits).
  if (from->isObjectType() || from->isFunctionType())
    from = sema.Context.getRValueReferenceType(from);
  OpaqueValueExpr from_expr{conv_loc, from.getNonLValueExprType(sema.Context),
                            Expr::getValueKindForType(from)};
  Expr* from_expr_ptr = &from_expr;
  EnterExpressionEvaluationContext unevaluated{
      sema, Sema::ExpressionEvaluationContext::Unevaluated};
  Sema::SFINAETrap sfinae_trap{sema, true};
  InitializedEntity to_entity = InitializedEntity::InitializeTemporary(to);
  InitializationKind init_kind =
      InitializationKind::CreateCopy(conv_loc, conv_loc);
  InitializationSequence init{sema, to_entity, init_kind, from_expr_ptr};
  if (init.Failed())
    return false;

  ExprResult result = init.Perform(sema, to_entity, init_kind, from_expr_ptr);
  return !result.isInvalid() && !sfinae_trap.hasErrorOccurred();
}

// --- Utilities for Stmt.

bool IsAddressOf(const Expr* expr) {
  if (const UnaryOperator* unary = DynCastFrom(expr->IgnoreParens()))
    return unary->getOpcode() == clang::UO_AddrOf;
  return false;
}

bool IsDependentNameCall(const Expr* expr) {
  const CallExpr* call_expr = dyn_cast<CallExpr>(expr);
  if (!call_expr)
    return false;
  return isa<UnresolvedLookupExpr>(call_expr->getCallee());
}

const Type* TypeOfParentIfMethod(const CallExpr* expr) {
  // callee_expr is a MemberExpr if we're a normal class method, or
  // DeclRefExpr if we're a static class method or an overloaded operator.
  const Expr* callee_expr = expr->getCallee()->IgnoreParenCasts();
  if (const MemberExpr* member_expr = DynCastFrom(callee_expr)) {
    const Type* class_type = GetTypeOf(member_expr->getBase());
    // For class->member(), class_type is a pointer.
    return RemovePointersAndReferencesAsWritten(class_type);
  } else if (const DeclRefExpr* ref_expr = DynCastFrom(callee_expr)) {
    if (ref_expr->getQualifier()) {    // static methods like C<T>::a()
      return ref_expr->getQualifier()->getAsType();
    }
  }
  return nullptr;
}

const Expr* GetFirstClassArgument(CallExpr* expr) {
  if (const FunctionDecl* callee_decl = expr->getDirectCallee()) {
    if (isa<CXXMethodDecl>(callee_decl)) {
      // If a method is called, return 'this'.
      return expr->getArg(0);
    }
    // Handle non-member functions.
    CHECK_(callee_decl->getNumParams() == expr->getNumArgs() &&
        "Require one-to-one match between call arguments and decl parameters");
    int params_count = callee_decl->getNumParams();
    for (int i = 0; i < params_count; i++) {
      // View argument types from the perspective of function declaration,
      // not from the caller's perspective.  For example, function parameter
      // can have template type but function argument is not necessarily
      // a template when the function is called.
      const Type* param_type = GetTypeOf(callee_decl->getParamDecl(i));
      param_type = RemovePointersAndReferencesAsWritten(param_type);
      // Make sure we do the right thing given a function like
      //    template <typename T> void operator>>(const T& x, ostream& os);
      // In this case ('myclass >> os'), we want to be returning the
      // type of os, not of myclass, and we do, because myclass will be
      // a SubstTemplateTypeParmType, not a RecordType.
      if (isa<SubstTemplateTypeParmType>(param_type))
        continue;
      // See through typedefs.
      param_type = param_type->getUnqualifiedDesugaredType();
      if (isa<RecordType>(param_type) ||
          isa<TemplateSpecializationType>(param_type)) {
        return expr->getArg(i);
      }
    }
  }
  return nullptr;
}

const CXXDestructorDecl* GetDestructorForDeleteExpr(const CXXDeleteExpr* expr) {
  const Type* type = expr->getDestroyedType().getTypePtrOrNull();
  // type is nullptr when deleting a dependent type: 'T foo; delete foo'
  if (type == nullptr)
    return nullptr;
  const NamedDecl* decl = TypeToDeclAsWritten(type);
  if (const CXXRecordDecl* cxx_record = DynCastFrom(decl))
    return cxx_record->getDestructor();
  return nullptr;
}

const CXXDestructorDecl* GetSiblingDestructorFor(
    const CXXConstructorDecl* ctor) {
  return ctor ? ctor->getParent()->getDestructor() : nullptr;
}

const CXXDestructorDecl* GetSiblingDestructorFor(
    const CXXConstructExpr* ctor_expr) {
  return GetSiblingDestructorFor(ctor_expr->getConstructor());
}

const FunctionType* GetCalleeFunctionType(CallExpr* expr) {
  const Type* callee_type = expr->getCallee()->getType().getTypePtr();
  if (const PointerType* ptr_type = callee_type->getAs<PointerType>()) {
    callee_type = ptr_type->getPointeeType().getTypePtr();
  } else if (const BlockPointerType* bptr_type =
                 callee_type->getAs<BlockPointerType>()) {
    callee_type = bptr_type->getPointeeType().getTypePtr();
  } else if (const MemberPointerType* mptr_type =
                 callee_type->getAs<MemberPointerType>()) {
    callee_type = mptr_type->getPointeeType().getTypePtr();
  }
  return callee_type->getAs<FunctionType>();
}

TemplateArgumentListInfo GetExplicitTplArgs(const Expr* expr) {
  TemplateArgumentListInfo explicit_tpl_args;
  if (const DeclRefExpr* decl_ref = DynCastFrom(expr))
    decl_ref->copyTemplateArgumentsInto(explicit_tpl_args);
  else if (const MemberExpr* member_expr = DynCastFrom(expr))
    member_expr->copyTemplateArgumentsInto(explicit_tpl_args);
  else if (const OverloadExpr* overload_expr = DynCastFrom(expr))
    overload_expr->copyTemplateArgumentsInto(explicit_tpl_args);
  else if (const DependentScopeDeclRefExpr* dependent_decl_ref = DynCastFrom(expr))
    dependent_decl_ref->copyTemplateArgumentsInto(explicit_tpl_args);
  return explicit_tpl_args;
}

string GetKindName(const Decl* decl) {
  return string(decl->getDeclKindName()) + "Decl";
}

string GetKindName(const Stmt* stmt) {
  return stmt->getStmtClassName();
}

string GetKindName(const Type* type) {
  return string(type->getTypeClassName()) + "Type";
}

string GetKindName(const TypeLoc typeloc) {
  return string(typeloc.getTypePtr()->getTypeClassName()) + "TypeLoc";
}

bool IsDeclaredInsideFunction(const Decl* decl) {
  const DeclContext* decl_ctx = decl->getDeclContext();
  return isa<FunctionDecl>(decl_ctx);
}

bool IsDeclaredInsideMacro(const Decl* decl) {
  SourceRange range = decl->getSourceRange();
  return range.getBegin().isMacroID() || range.getEnd().isMacroID();
}

}  // namespace include_what_you_use
