#include "llvm/Support/CommandLine.h"

#include "clang/Frontend/CompilerInstance.h"
#include "clang/Frontend/FrontendAction.h"
#include "clang/AST/RecursiveASTVisitor.h"
#include "clang/Rewrite/Core/Rewriter.h"
#include "clang/Tooling/CommonOptionsParser.h"
#include "clang/Tooling/Tooling.h"

#include <iostream>

#include <clang/AST/ASTContext.h>

#include "clang/ASTMatchers/ASTMatchers.h"
#include "clang/ASTMatchers/ASTMatchFinder.h"

using namespace clang;
using namespace clang::tooling;
using namespace clang::ast_matchers;
using namespace llvm;

static cl::OptionCategory MyToolCategory("My tool options");
static cl::opt<std::string> MyToolOutput("o", cl::desc("Output file"), cl::cat(MyToolCategory));
static cl::opt<bool> LoopsVectorization("lv", cl::desc("Loops vectorization"), cl::cat(MyToolCategory));


const char loops[] = "for (Int _j = 0; _j < _sizeY; _j++) {\n"
                     "for (Int _i = 0; _i < _sizeX; _i++) {\n"
                     "_evaluate(_i, _j, Real());\n"
                     "}\n"
                     "}\n";


const char vecLoops[] =
        "for (Int _j = 0; _j < _sizeY; _j++) {\n"
        "Int _i = 0;\n"
        "for (; _i < _sizeX / 4 * 4; _i+=4) {\n"
        "_evaluate(_i, _j, Real4());\n"
        "}\n"
        "for (; _i < _sizeX; _i++) {\n"
        "_evaluate(_i, _j, Real());\n"
        "}\n"
        "}\n";

std::string escapedName(std::string name) {
    for (char& c : name) {
        if (!std::isalnum(c)) {
            switch (c) {
            case '-': c = 'm'; break;
            case '+': c = 'p'; break;
            default: c = '_';
            }
        }
    }
    return '_' + name;
}

std::string temporaryName(const std::string& name) {
    return 't' + escapedName(name);
}

std::string getText(Stmt* stmt, Rewriter& rewriter) {
    assert(stmt);
    return Lexer::getSourceText(
            CharSourceRange::getTokenRange(stmt->getSourceRange()),
            rewriter.getSourceMgr(),
            LangOptions());
}

class SequenceVisitor : public RecursiveASTVisitor<SequenceVisitor> {
public:
    SequenceVisitor(Rewriter& rewriter)
        : mRewriter(rewriter)
    {

    }

    bool TraverseStmt(Stmt* e) {
        if (e && isa<Expr>(e) &&
                (cast<Expr>(e)->getType().getAsString() == "struct Var" ||
                 cast<Expr>(e)->getType().getAsString() == "struct Array"))
        {
            std::string text = getText(e, mRewriter);
            
            if(isOnlyWriteInBuffer == false) {
                mRewriter.ReplaceText(e->getSourceRange(), escapedName(text));
            }
            sourceBuffer.push_back(escapedName(text));

            rhsVars.insert(text);
            
            return true;
        } else if (e && isa<Expr>(e) && cast<Expr>(e)->getType().getAsString() == "struct Temp") {
            std::string tempName = getText(e, mRewriter);
            temps.insert(tempName);

            if(isOnlyWriteInBuffer == false) {
                mRewriter.ReplaceText(e->getSourceRange(), escapedName(tempName));
            }
            sourceBuffer.push_back(escapedName(tempName));

            return true;
        } else if (e && isa<CXXOperatorCallExpr>(e) &&
                   cast<CXXOperatorCallExpr>(e)->getType().getAsString() == "struct Assign") {
            auto ee = cast<CXXOperatorCallExpr>(e);
            assert(ee->getNumArgs() == 2);

            Expr* lhs = ee->getArg(0);
        
            assert(lhs->getType().getAsString() == "struct Array" ||
                lhs->getType().getAsString() == "struct Var" ||
                lhs->getType().getAsString() == "struct Temp");

            std::string lhsText = getText(lhs, mRewriter);

            if(isOnlyWriteInBuffer == false) {
                mRewriter.ReplaceText(lhs->getSourceRange(), escapedName(lhsText));
            }
            sourceBuffer.push_back(escapedName(lhsText));
        
            if (lhs->getType().getAsString() == "struct Array" || 
                lhs->getType().getAsString() == "struct Var") 
            {
                lhsVars.insert(lhsText);
            }
            
            return RecursiveASTVisitor<SequenceVisitor>::TraverseStmt(ee->getArg(1));
        } else if(e && isa<LambdaExpr>(e))
        {
            LambdaExpr* lambdaExpr = cast<LambdaExpr>(e);

            std::string s = Lexer::getSourceText(
                        CharSourceRange::getTokenRange(SourceRange(lambdaExpr->getBeginLoc(), lambdaExpr->getBody()->getLBracLoc().getLocWithOffset(-1))),
                        mRewriter.getSourceMgr(),
                        LangOptions());

            lambdaFunctions.push_back("auto loop = " + s);
            lambdaStmt.push_back(lambdaExpr->getBody());
            return true;
        } else {
            return RecursiveASTVisitor<SequenceVisitor>::TraverseStmt(e);
        }

    }

    bool VisitDeclRefExpr(DeclRefExpr* e) {
        if (e->getNameInfo().getAsString() == "operator,") {
            if(isOnlyWriteInBuffer == false) {
                mRewriter.ReplaceText(e->getSourceRange(), ";");
            }
            sourceBuffer.push_back(";");
        }
        return true;
    }

    std::set<std::string> allVars() {
        std::set<std::string> res;
        res.insert(lhsVars.begin(), lhsVars.end());
        res.insert(rhsVars.begin(), rhsVars.end());
        return res;
    }

    std::set<std::string> rhsVars; // each use of Var
    std::set<std::string> lhsVars; // each use of Var
    std::set<std::string> temps; // each use of Temp

    std::vector<CompoundStmt*> lambdaStmt;
    std::vector<std::string> lambdaFunctions;

    std::vector<std::string> sourceBuffer;
    bool isOnlyWriteInBuffer = false;

private:
    Rewriter& mRewriter;
};

template <typename T>
std::vector<T*> findCurrentChildrens(Stmt* parent) {
    std::vector<T*> findedChildrens;
    auto parentChildrens =  parent->children();

    if(parentChildrens.begin() != parentChildrens.end()) {
        for(Stmt* parentChild : parentChildrens) {
            if(isa<T>(parentChild)) {
                T* findedChild = cast<T>(parentChild);
                findedChildrens.push_back(findedChild);
            }
            if(parentChild->children().begin() != parentChild->children().end()) {
                std::vector<T*> childChildrens = findCurrentChildrens<T>(parentChild);
                findedChildrens.insert(findedChildrens.end(), childChildrens.begin(), childChildrens.end());
            }
        }
    }
    return findedChildrens;
}

template <typename T>
T* findCurrentChild(Stmt* parent) {
    T* findedChild = nullptr;
    auto parentChildrens =  parent->children();

    if(parentChildrens.begin() != parentChildrens.end()) {
        for(Stmt* parentChild : parentChildrens) {
            if(parentChild && isa<T>(parentChild)) {
                return dyn_cast<T>(parentChild);
            }
            if(parentChild->children().begin() != parentChild->children().end()) {
                findedChild = findCurrentChild<T>(parentChild);
                if(findedChild != nullptr) return findedChild;
            }
        }
    }
    return findedChild;
}

class MyASTVisitor : public RecursiveASTVisitor<MyASTVisitor> {
public:
    MyASTVisitor(Rewriter& rewriter, ASTContext* context)
        : mRewriter(rewriter)
        , mContext(context)
    {
    }

    bool VisitVarDecl(VarDecl *v) {
        if (v->getType().getAsString() == "struct Temp") {
            mRewriter.RemoveText(v->getSourceRange());
        }
        return true;
    }

    bool VisitDeclRefExpr(DeclRefExpr* e) {
        if(e->getType().getAsString() == "struct Size") {
            auto r = e->getSourceRange();
            mRewriter.ReplaceText(r, "_sizeX, _sizeY");
        }
        return true;
    }

    bool VisitFunctionDecl(FunctionDecl *f) {
        bool exprDetected = false;
        if (f->getReturnType().getAsString() == "struct Expr") {
            mRewriter.ReplaceText(f->getReturnTypeSourceRange(), "auto");
        }

        int templatesNum = 0;

        for (unsigned i = 0; i < f->getNumParams(); i++) {
            ParmVarDecl* p = f->getParamDecl(i);
            if (p->getType().getAsString() == "struct Expr") {
                mRewriter.ReplaceText(p->getTypeSourceInfo()->getTypeLoc().getSourceRange(),
                                      "E" + std::to_string(templatesNum));
                templatesNum += 1;
            }
        }
        if (templatesNum > 0) {
            mRewriter.InsertText(f->getBeginLoc(), "template <", true, true);
            for (int i = 0; i < templatesNum; i++) {
                mRewriter.InsertText(f->getBeginLoc(), "typename E" + std::to_string(i), true, true);
                if (i != templatesNum - 1) {
                    mRewriter.InsertText(f->getBeginLoc(), ", ", true, true);
                }
            }
            mRewriter.InsertText(f->getBeginLoc(), ">\n", true, true);
        }
        return true;
    }

    bool StmtProcessing(Stmt* s, Stmt* loopStmt = nullptr, int stmtIndex = -1) {
        Expr* expr = cast<Expr>(s);
        assert(expr);

        // find begin and end
        SourceLocation begin = expr->getBeginLoc();
        SourceLocation end = expr->getEndLoc();

        // find length of last token
        unsigned endOffset = Lexer::MeasureTokenLength(
                    end, mRewriter.getSourceMgr(), mRewriter.getLangOpts());
        endOffset++; // skip ';' after last token
        end = end.getLocWithOffset(endOffset);

        // process expression
        SequenceVisitor sv(mRewriter);
        sv.TraverseStmt(expr);

        if (sv.allVars().empty()) {
            // sometimes it can be empty (in headers)
            // but if it is empty in our code we should stop on error
            mRewriter.InsertText(begin, "!!! ``` !!!", true, true);
            return true;
        }

        if(loopStmt == nullptr)
            mRewriter.InsertText(begin, "{\n", true, true);

        // put all rhs Vars into temporaries
        for (auto& e : sv.allVars()) {
            std::string ss;
            if(stmtIndex >= 0) {
                ss = (Twine("Var ") + temporaryName(e) + "_" + std::to_string(stmtIndex) + " = " + e + ";\n").str();
            } else ss = (Twine("Var ") + temporaryName(e) + " = " + e + ";\n").str();
            mRewriter.InsertText(begin, ss, true, true);
        }

        // find iteration space size
        std::string sizeXStr="";
        std::string sizeYStr="";
        switch (stmtIndex) {
        case 0: //create sizeX sizeY one time for block of sequences
            sizeXStr = (Twine("Int _sizeX") + " = " + temporaryName(*sv.allVars().begin())
                             + "_" + std::to_string(stmtIndex) + ".range().size().x;\n").str();
            sizeYStr = (Twine("Int _sizeY") + " = " + temporaryName(*sv.allVars().begin())
                             + "_" + std::to_string(stmtIndex) + ".range().size().y;\n").str();
            break;
        case -1: // default state
            sizeXStr = (Twine("Int _sizeX") + " = " + temporaryName(*sv.allVars().begin())
                        + ".range().size().x;\n").str();
            sizeYStr = (Twine("Int _sizeY") + " = " + temporaryName(*sv.allVars().begin())
                        + ".range().size().y;\n").str();
            break;
        default: // dont create sizeX sizeY
            break;
        }

        mRewriter.InsertText(begin, sizeXStr, true, true);
        mRewriter.InsertText(begin, sizeYStr, true, true);

        // add assertions
        for (auto& e : sv.allVars()) {
            std::string ssX;
            std::string ssY;
            if(stmtIndex >= 0) {
                    ssX = (Twine("assert(") +  temporaryName(e) + "_" + std::to_string(stmtIndex) + ".range().size().x == _sizeX);\n").str();
                    ssY = (Twine("assert(") +  temporaryName(e) + "_" + std::to_string(stmtIndex) + ".range().size().y == _sizeY);\n").str();
            } else {ssX = (Twine("assert(") +  temporaryName(e) + ".range().size().x == _sizeX);\n").str();
                    ssY = (Twine("assert(") +  temporaryName(e) + ".range().size().y == _sizeY);\n").str();}
            mRewriter.InsertText(begin, ssX, true, true);
            mRewriter.InsertText(begin, ssY, true, true);
        }

        if(loopStmt == nullptr) {
         // add expression lambda
        mRewriter.InsertText(begin, "auto _evaluate = [&](Int _i, Int _j, auto type) __attribute__((always_inline)) {\n", true, true);
        } else {
          std::string lamdbaDecl = (Twine("auto _evaluate_") + std::to_string(stmtIndex) + "= [&](Int _i, Int _j, auto type) __attribute__((always_inline)) {\n").str();
          mRewriter.InsertText(begin, lamdbaDecl, true, true);
        }
        mRewriter.InsertText(begin, "using T = decltype(type);\n", true, true);

        // create temporaries for each loop iteration
        mRewriter.InsertText(begin, "// temporaries\n", true, true);
        for (auto& e : sv.temps) {
            std::string ss = (Twine("T ") + escapedName(e) + ";\n").str();
            mRewriter.InsertText(begin, ss, true, true);
        }

        mRewriter.InsertText(begin, "// variables\n", true, true);
        for (auto& e : sv.allVars()) {
            std::string ss = (Twine("T ") + escapedName(e) + ";\n").str();
            mRewriter.InsertText(begin, ss, true, true);
        }

        // load values from both lhs and rhs
        // we load lhs, because it is difficult to detect is it read only or read write
        mRewriter.InsertText(begin, "// loads\n", true, true);
        for (auto& e : sv.allVars()) {
            std::string ss;
            if(stmtIndex >= 0) {
                ss = (Twine("loadFromPtr(") + escapedName(e) + ", " +
                      "&" + temporaryName(e) + "_" + std::to_string(stmtIndex) + ".val(_i, _j));\n").str();
            } else ss = (Twine("loadFromPtr(") + escapedName(e) + ", " +
                         "&" + temporaryName(e) + ".val(_i, _j));\n").str();
            mRewriter.InsertText(begin, ss, true, true);
        }

        // here the main expressions will be inserted
        // add end of line after it
        mRewriter.InsertText(end, "\n", true, true);

        // store values to lhs
        for (auto& e : sv.lhsVars) {
            std::string ss;
            if(stmtIndex >= 0) {
                ss = (Twine("storeToPtr(") +
                      "&" + temporaryName(e) + "_" + std::to_string(stmtIndex) + ".val(_i, _j)," +
                      escapedName(e) + ");\n").str();
            } else ss = (Twine("storeToPtr(") +
                         "&" + temporaryName(e) + ".val(_i, _j)," +
                         escapedName(e) + ");\n").str();
            mRewriter.InsertText(end, ss, true, true);
        }

        // add closing brace of lambda
        mRewriter.InsertText(end, "};\n", true, true);

        if(loopStmt == nullptr) {
            if (LoopsVectorization.getValue()) {
                mRewriter.InsertText(end, vecLoops, true, true);
            } else {
                mRewriter.InsertText(end, loops, true, true);
            }
        }

        if(loopStmt == nullptr)
            mRewriter.InsertText(end, "}\n", true, true);

        return true;
    }

    std::vector<Expr*> findCurrentExprInStmt (Stmt* f, std::string name) {
        std::vector<Expr*> findedExpr;

        for(auto child : findCurrentChildrens<Expr>(f)) {
            if(child && child->getType().getAsString() == name) {
                findedExpr.push_back(cast<Expr>(child));
            }
        }
        return findedExpr;
    }

    //DefineLoop() processing
    std::vector<std::string> loopProcessing(CompoundStmt* loop) {
        std::vector<std::string> sourceBuffer;

        SourceLocation stmtStart = loop->getBeginLoc();
        SourceLocation stmtEnd = loop->getEndLoc();

        std::vector<Expr*> loopSeqVec = findCurrentExprInStmt(loop, "class LoopSequence");

        for(auto l : loopSeqVec) {
            CallExpr* callInitLoop = cast<CallExpr>(l);
            SourceLocation loopInitStart =Lexer::GetBeginningOfToken(callInitLoop->getBeginLoc().getLocWithOffset(-1), mRewriter.getSourceMgr(), LangOptions());
            std::string s = Lexer::getSourceText(
                        CharSourceRange::getTokenRange(SourceRange(stmtStart, loopInitStart)),
                        mRewriter.getSourceMgr(),
                        LangOptions());

                sourceBuffer.push_back(s);

            std::string loopNum =  getText(callInitLoop->getArg(0), mRewriter);
            std::string vectorizationNum = getText(callInitLoop->getArg(1), mRewriter);
            std::string i = getText(callInitLoop->getArg(2), mRewriter);
            std::string j = getText(callInitLoop->getArg(3), mRewriter);

            std::string lambdaDecl;
            if(std::stoi(vectorizationNum) == 1) {
                lambdaDecl = (Twine("_evaluate_")+ loopNum + "(" + i + ", " + j + ", " + "Real())").str();
            } else {
                lambdaDecl = (Twine("_evaluate_")+ loopNum + "(" + i + ", " + j + ", " + "Real" + vectorizationNum + "())").str();
            }
            sourceBuffer.push_back(lambdaDecl);

            stmtStart = Lexer::GetBeginningOfToken(callInitLoop->getEndLoc().getLocWithOffset(1), mRewriter.getSourceMgr(), LangOptions());
        }

        sourceBuffer.push_back(Lexer::getSourceText(
                                   CharSourceRange::getTokenRange(SourceRange(stmtStart, stmtEnd)),
                                   mRewriter.getSourceMgr(),
                                   LangOptions()));
        return sourceBuffer;
    }

    bool TraverseStmt(Stmt* e) {
        if (e && isa<Expr>(e) &&
                (cast<Expr>(e)->getType().getAsString() == "struct Sequence" ||
                 cast<Expr>(e)->getType().getAsString() == "struct Assign"))
        {
            return StmtProcessing(e);

        }  else if (e && isa<CXXOperatorCallExpr>(e) && cast<CXXOperatorCallExpr>(e)->getType().getAsString() == "class DefineLoop") {
            CXXOperatorCallExpr* expr = cast<CXXOperatorCallExpr>(e);
            assert(expr);

            //find lambda expressions
            SequenceVisitor sm(mRewriter);
            sm.TraverseStmt(expr);

            SourceLocation begin = Lexer::GetBeginningOfToken(expr->getBeginLoc(), mRewriter.getSourceMgr(), mRewriter.getLangOpts());

            //first is defineloop; Need to check in
            CompoundStmt* defineLoopStmt = sm.lambdaStmt.at(0);

            //remove DefineLoop
            SourceLocation prevStmtEndLoc = defineLoopStmt->getRBracLoc();
            mRewriter.RemoveText(SourceRange(begin, prevStmtEndLoc));

            std::vector<Stmt*> statements;
            for(CompoundStmt* stmt : sm.lambdaStmt) {
                for(Stmt* child : stmt->children()) {
                    if(isa<Expr>(child) &&(cast<Expr>(child)->getType().getAsString() == "struct Sequence" ||
                                           cast<Expr>(child)->getType().getAsString() == "struct Assign"))  {
                        statements.push_back(child);

                        //remove brackets and operator+ between lambdas
                        SourceLocation currentStmtBeginLoc = stmt->getLBracLoc();
                        mRewriter.RemoveText(SourceRange(prevStmtEndLoc, currentStmtBeginLoc));
                        prevStmtEndLoc = stmt->getRBracLoc();
                    }
                }
            }

           mRewriter.InsertText(begin, "{\n", true, true);

            int stmtNum = 0;
            for (auto stmt : statements) {
                StmtProcessing(stmt, defineLoopStmt, stmtNum);
                stmtNum++;
            }

            mRewriter.InsertText(expr->getEndLoc(), Twine(sm.lambdaFunctions.at(0) + " __attribute__((always_inline)) { \n").str(), true, true);

            for(auto str : loopProcessing(defineLoopStmt)) {
                mRewriter.InsertText(expr->getEndLoc(), Twine(str).str(), true, true);
            }
            mRewriter.InsertText(expr->getEndLoc(), Twine("};\n").str(), true, true);

            mRewriter.InsertText(expr->getEndLoc(), Twine("loop(_sizeX, _sizeY);\n").str(), true, true);

            return true;
        } else if(e && isa<DeclStmt>(e))
        {
            DeclStmt* declStmt = dyn_cast<DeclStmt>(e);
            DeclGroupRef declGroup = declStmt->getDeclGroup();

            //DeclStmt has childrens - VarDecl (ex. int i, j, k; => three VarDecl)
            for(auto decl : declGroup) {
                if(isa<VarDecl>(decl) && dyn_cast<VarDecl>(decl)->getType().getAsString() == "struct Size"
                        && dyn_cast<VarDecl>(decl)->getAnyInitializer() != nullptr) {
                    auto r = e->getSourceRange();
                    VarDecl* v = dyn_cast<VarDecl>(decl);

                    Expr* e = v->getInit();//v->getAnyInitializer();
                    std::vector<DeclRefExpr*> refExpr = findCurrentChildrens<DeclRefExpr>(cast<Stmt>(e));

                    std::vector<std::string> rValueNames;
                    for (auto ref : refExpr) {
                        std::string s = Lexer::getSourceText(
                                    CharSourceRange::getTokenRange(ref->getSourceRange()),
                                    mRewriter.getSourceMgr(),
                                    LangOptions());
                        rValueNames.push_back(s);
                    }

                    auto sizeXStr = (Twine("Int _sizeX = ") + rValueNames[0] + ";\n").str();
                    auto sizeYStr = (Twine("Int _sizeY = ") + rValueNames[1] + ";\n").str();
                    mRewriter.ReplaceText(r, sizeXStr + sizeYStr);
                }
            }
            return RecursiveASTVisitor<MyASTVisitor>::TraverseStmt(e);

        } else {
            return RecursiveASTVisitor<MyASTVisitor>::TraverseStmt(e);
        }
        return true;
    }

private:
    Rewriter& mRewriter;
    ASTContext* mContext;
};

class ForLoopHandler : public MatchFinder::MatchCallback {
public:
    ForLoopHandler(Rewriter& rewriter) : mRewriter(rewriter) {}

    virtual void run(const MatchFinder::MatchResult& result) {

    }

private:
    Rewriter& mRewriter;
};

class MyASTConsumer : public ASTConsumer {
public:
    MyASTConsumer(Rewriter& rewriter, ASTContext *context)
        : mVisitor(rewriter, context)
    {
        //mMatcher.addMatcher(..., &mForLoopHandler);
//        mMatcher.addMatcher(
//                    expr(hasName("struct DefineLoop")
//                         ));
//        Matcher.addMatcher(
//              forStmt(hasLoopInit(declStmt(hasSingleDecl(
//                         varDecl(hasInitializer(integerLiteral(equals(0))))
//                              .bind("initVarName")))),
//                      hasIncrement(unaryOperator(
//                          hasOperatorName("++"),
//                          hasUnaryOperand(declRefExpr(to(
//                              varDecl(hasType(isInteger())).bind("incVarName")))))),
//                      hasCondition(binaryOperator(
//                          hasOperatorName("<"),
//                          hasLHS(ignoringParenImpCasts(declRefExpr(to(
//                              varDecl(hasType(isInteger())).bind("condVarName"))))),
//                          hasRHS(expr(hasType(isInteger()))))))
//                  .bind("forLoop"),
//      &HandlerForFor);
    }

    bool HandleTopLevelDecl(DeclGroupRef dr) override {
        for (DeclGroupRef::iterator b = dr.begin(); b != dr.end(); ++b) {
            mVisitor.TraverseDecl(*b);
        }
        return true;
    }

    void HandleTranslationUnit(ASTContext& context) override {
        mMatcher.matchAST(context);
    }

private:
    MyASTVisitor mVisitor;
    MatchFinder mMatcher;
};

class MyFrontendAction : public ASTFrontendAction {
public:
    void EndSourceFileAction() override {
        SourceManager &sm = mRewriter.getSourceMgr();

        StringRef fname = sm.getFileEntryForID(sm.getMainFileID())->getName();
        llvm::errs() << "** EndSourceFileAction for: " << fname << "\n";

        std::error_code error_code;
        llvm::raw_fd_ostream outFile(MyToolOutput.getValue(), error_code,
                                     llvm::sys::fs::OF_None);
        mRewriter.getEditBuffer(sm.getMainFileID()).write(outFile);
    }

    std::unique_ptr<ASTConsumer> CreateASTConsumer(
            CompilerInstance& ci, StringRef file) override
    {
        llvm::errs() << "** Creating AST consumer for: " << file << "\n";
        mRewriter.setSourceMgr(ci.getSourceManager(), ci.getLangOpts());
        return llvm::make_unique<MyASTConsumer>(mRewriter, &ci.getASTContext());
    }
private:
    Rewriter mRewriter;
};

int main(int argc, const char **argv) {
    CommonOptionsParser op(argc, argv, MyToolCategory);
    ClangTool Tool(op.getCompilations(), op.getSourcePathList());

    return Tool.run(newFrontendActionFactory<MyFrontendAction>().get());
}
