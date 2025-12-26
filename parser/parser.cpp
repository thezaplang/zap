#include "parser.hpp"
#include "../ast/fun_call.hpp"
#include "../ast/const/const_id.hpp"
#include "parser.hpp"
#include <vector>

std::unique_ptr<RootNode> Parser::parse(std::vector<Token> toks){
    this->pos_= 0;
    this->toks_ = toks;
    auto root = std::make_unique<RootNode>();
    while(!isAtEnd()){
        Token current = peek();

        if(current.type == TokenType::FUN || current.type == TokenType::EXTERN){
            root->addChild(parseFun());
        }
        else {
            printf("Unexpected token: %s at position %d\n", current.value.c_str(), current.pos);
            exit(-1); // Handle unexpected tokens
        }
    }
    return root;
}
std::unique_ptr<FunDecl> Parser::parseFun() {
    auto func = std::make_unique<FunDecl>();
    if(peek().type == TokenType::EXTERN) {
        func->isExtern_ = true;
        advance(); // consume 'extern'
    }
    if(peek().type == TokenType::STATIC) {
        func->isStatic_ = true;
        advance(); // consume 'static'
    }
    if(peek().type == TokenType::PUB) {
        func->isPublic_ = true;
        advance(); // consume 'pub'
    }
    consume(TokenType::FUN);
    func->name_ = consume(TokenType::ID, "Expected function name.").value;

    consume(TokenType::LPAREN, "Expected '(' after function name.");
    func->params_ = parseParameters();
    consume(TokenType::RPAREN, "Expected ')' after params got: " + peek().value);


    if (peek().type == TokenType::MINUS) {
        advance(); 
        if (peek().type == TokenType::GREATER) {
            consume(TokenType::GREATER, "Expected '>' after '-' in return type.");
            func->returnType_ = parseType();
        } else {
            printf("Expected '>' after '-' in return type.\n");
            exit(-1);
        }
    }
    if(peek().type == TokenType::SEMICOLON) {
        if(!func->isExtern_) {
            printf("Only extern functions can omit the body.\n");
            exit(-1);
        }
        consume(TokenType::SEMICOLON);

  
        return func;
    }
    func->body_ = parseBody();
    return func;
}

std::unique_ptr<VarDecl> Parser::parseVarDecl() {
    /*
        let name = <expr>;
        let name: type;
        let name: type = <expr>;
    */
    consume(TokenType::LET);
    
    std::string id = consume(TokenType::ID, "Expected variable name.").value;
    if (peek().type == TokenType::COLON) {
		advance(); // consume ':'
		auto type = parseType();
        if (peek().type == TokenType::ASSIGN) {
            advance(); 
			// consume '='
			auto expr = parseExpression();
			consume(TokenType::SEMICOLON, "Expected ';' after variable declaration.");
            return std::make_unique<VarDecl>(id, std::move(type), std::move(expr));
        } else {
			consume(TokenType::SEMICOLON, "Expected ';' after variable declaration.");
            return std::make_unique<VarDecl>(id, std::move(type), nullptr);
		}
    }
    else {
        consume(TokenType::ASSIGN, "Expected '=' after variable name.");
        auto expr = parseExpression();
        consume(TokenType::SEMICOLON, "Expected ';' after variable declaration.");
        return std::make_unique<VarDecl>(id, nullptr, std::move(expr));
    }
}

std::unique_ptr<ExpressionNode> Parser::parseExpression() {
    return parseTerm();
}



std::unique_ptr<ExpressionNode> Parser::parseTerm() {
	std::unique_ptr<ExpressionNode> left = parseFactor();
    while (peek().type == TokenType::PLUS || peek().type == TokenType::MINUS) {
        Token operatorToken = next();
        std::unique_ptr<ExpressionNode> right = parseFactor();
        left = std::make_unique<BinExpr>(std::move(left), operatorToken.value, std::move(right));
	}

    return left;
}

std::unique_ptr<ExpressionNode> Parser::parseFactor() {
    std::unique_ptr<ExpressionNode> left = parseUnary();
    while (peek().type == TokenType::MULTIPLY || peek().type == TokenType::DIVIDE || peek().type == TokenType::AND) {
        Token operatorToken = next();
        std::unique_ptr<ExpressionNode> right = parseUnary();
        left = std::make_unique<BinExpr>(std::move(left), operatorToken.value, std::move(right));
    }
    return left;
}
std::unique_ptr<ExpressionNode> Parser::parseIntConst(Token current) {
    return std::unique_ptr<ExpressionNode>(std::make_unique<ConstInt>(std::stoi(current.value)).release());
}

std::unique_ptr<ExpressionNode> Parser::parseConstId(Token current) {
    return std::unique_ptr<ExpressionNode>(std::make_unique<ConstId>(current.value).release());
}

std::unique_ptr<ExpressionNode> Parser::parseUnary() {
    if (peek().type == TokenType::MINUS || peek().type == TokenType::NOT || peek().type == TokenType::MULTIPLY || peek().type == TokenType::REFERENCE) {
        Token operatorToken = next();
        std::unique_ptr<ExpressionNode> right = parseUnary();
        
        if (operatorToken.type == TokenType::MULTIPLY) {
            return std::make_unique<UnaryExpr>("*", std::move(right));
        }
 
        return std::make_unique<UnaryExpr>(operatorToken.value, std::move(right));
    }
    return parsePrimary();
}

std::unique_ptr<ExpressionNode> Parser::parsePrimary() {
    Token current = peek();
    if (current.type == TokenType::INTEGER) {
        advance();
        return std::unique_ptr<ExpressionNode>(std::make_unique<ConstInt>(std::stoi(current.value)));
    } else if (current.type == TokenType::FLOAT) {
        advance();
        return std::unique_ptr<ExpressionNode>(std::make_unique<ConstFloat>(std::stof(current.value)));
    } else if(current.type == TokenType::STRING){
        advance();
        return std::unique_ptr<ExpressionNode>(std::make_unique<ConstString>(current.value));
    }
    else if(current.type == TokenType::BOOL){
        advance();
        if(current.value == "true"){
            return std::unique_ptr<ExpressionNode>(std::make_unique<ConstInt>(1, "i1"));
        }else {
            return std::unique_ptr<ExpressionNode>(std::make_unique<ConstInt>(0, "i1"));
        }
    }
    else if (current.type == TokenType::LPAREN) {
        advance(); // consume '('
        std::unique_ptr<ExpressionNode> expr = parseExpression();
        consume(TokenType::RPAREN, "Expected ')' after expression.");
        return expr;
    } else if (current.type == TokenType::ID) {

        Token idToken = current;
        advance();
        if (peek().type == TokenType::LPAREN) {
            advance(); // consume '('
            std::vector<std::unique_ptr<ExpressionNode>> args;
            if (peek().type != TokenType::RPAREN) {
                do {
                    args.push_back(parseExpression());
                } while (peek().type == TokenType::COMMA && (advance(), true));
            }
            consume(TokenType::RPAREN, "Expected ')' after function call arguments.");
            auto call = std::make_unique<FunCall>();
            call->funcName_ = idToken.value;
            call->arguments_ = std::move(args);
            return call;
        } else {
            return std::unique_ptr<ExpressionNode>(std::make_unique<ConstId>(idToken.value));
        }
    } else {
        printf("Unexpected token in primary expression: %s at position %d\n", current.value.c_str(), current.pos);
        exit(-1); // Handle unexpected tokens
    }
}

std::unique_ptr<BodyNode> Parser::parseBody(){
    auto body = std::make_unique<BodyNode>();
    consume(TokenType::LBRACE);
    while(!isAtEnd() && peek().type != TokenType::RBRACE){
		body->addStatement(parseStatement());
    }
    consume(TokenType::RBRACE, "Expected '}' when funtion ends.");
    return body;
}

std::unique_ptr<StatementNode> Parser::parseStatement() {
    if(peek().type == TokenType::LET){
        return parseVarDecl();
    }
    else if(peek().type == TokenType::RETURN){
        return parseReturn();
    }
    else if(peek().type == TokenType::BREAK){
        Token breakToken = consume(TokenType::BREAK);
        consume(TokenType::SEMICOLON, "Expected ';' after break statement.");
        return std::make_unique<BreakNode>();
    }
    else if(peek().type == TokenType::CONTINUE){
        Token continueToken = consume(TokenType::CONTINUE);
        consume(TokenType::SEMICOLON, "Expected ';' after continue statement.");
        return std::make_unique<ContinueNode>();
    }
    else if(peek().type == TokenType::ID && peek(1).type == TokenType::LPAREN) {
      
        Token idToken = consume(TokenType::ID);
        consume(TokenType::LPAREN, "Expected '(' after function name.");
        std::vector<std::unique_ptr<ExpressionNode>> args;
        if (peek().type != TokenType::RPAREN) {
            do {
                args.push_back(parseExpression());
            } while (peek().type == TokenType::COMMA && (advance(), true));
        }
        consume(TokenType::RPAREN, "Expected ')' after function call arguments.");
        consume(TokenType::SEMICOLON, "Expected ';' after function call statement.");
        auto call = std::make_unique<FunCall>();
        call->funcName_ = idToken.value;
        call->arguments_ = std::move(args);

        return std::move(call);
    }
    else {
        printf("Unexpected token in statement: %s at position %d\n", peek().value.c_str(), peek().pos);
        exit(-1); // Handle unexpected tokens
    }
}

std::unique_ptr<ReturnNode> Parser::parseReturn() {
    consume(TokenType::RETURN);
    if(peek().type == TokenType::SEMICOLON) {
        consume(TokenType::SEMICOLON, "Expected ';' after return statement.");
        return std::make_unique<ReturnNode>(nullptr);
    }
    std::unique_ptr<ExpressionNode> returnValue = parseExpression();
    consume(TokenType::SEMICOLON, "Expected ';' after return statement.");
    return std::make_unique<ReturnNode>(std::move(returnValue));
}
std::vector<std::unique_ptr<ParameterNode>> Parser::parseParameters(){
    std::vector<std::unique_ptr<ParameterNode>> params;
        while (true) {
            if (peek().type == TokenType::ID) {
        Token name = consume(TokenType::ID);
        consume(TokenType::COLON, "Expected ':' after param name.");
        std::unique_ptr<TypeNode> type = parseType();
        auto paramNode = std::make_unique<ParameterNode>();
        paramNode->name = name.value;
        paramNode->type = std::move(type);
        params.push_back(std::move(paramNode));
            } else if(peek().type == TokenType::ELLIPSIS) {
                consume(TokenType::ELLIPSIS);
                auto paramNode = std::make_unique<ParameterNode>();
                paramNode->name = "...";
                auto typeNode = std::make_unique<TypeNode>("varargs");
                typeNode->isVarArgs = true;
                paramNode->type = std::move(typeNode);
                params.push_back(std::move(paramNode));
            }
            else {
                break;
            }
            if (peek().type == TokenType::COMMA) {
                advance();
            } else {
                break;
            }
    }
    return params;
}

std::unique_ptr<TypeNode> Parser::parseType(){
    bool isPointer = false;
    bool isReference = false;
    bool isVarArgs = false;
    //for now only ID
    //todo: [], [size] for example [4], <T>
    if(peek().type == TokenType::MULTIPLY){
        consume(TokenType::MULTIPLY);
        isPointer = true;
    }
    else if(peek().type == TokenType::REFERENCE){
        consume(TokenType::REFERENCE);
        isReference = true;
    }

   
    if (peek().type == TokenType::ELLIPSIS) { 
        consume(TokenType::ELLIPSIS);
        isVarArgs = true;
    }

    auto typeName = consume(TokenType::ID, "type must be an identifier. not " + peek().value);
    TypeNode type = TypeNode(typeName.value);
    if(isPointer) {
        type.isPointer = true;
    }else if(isReference) {
        type.isReference = true;
    }
    if(isVarArgs) {
        type.isVarArgs = true; 
    }
    return std::make_unique<TypeNode>(type);
}
Token Parser::consume(TokenType expected, std::string err_msg){
    Token tok = peek();

    if(tok.type != expected){
        printf("%s\n", err_msg.c_str());//todo: add errors to list to allow multiple error handling
        exit(-1);
    }else {
    pos_++;
    return tok;
    }
}

void Parser::advance() {
    if (!isAtEnd()) {
        pos_++;
    }
    else {
        printf("parser can't advance! \n");
    }
}

Token Parser::next() {
    if (!isAtEnd()) {
        return toks_[pos_++];
    }
    else {
        printf("parser can't get next! \n");
        exit(-1); //todo: add errors to list to allow multiple error handling
    }
}

Token Parser::peek() {
    if (!isAtEnd()) {
        return toks_[pos_];
    } else {
        printf("parser can't peek! \n");
        exit(-1);
    }
}

Token Parser::peek(int offset) {
    if (pos_ + offset < toks_.size()) {
        return toks_[pos_ + offset];
    } else {
        printf("parser can't peek(offset)! \n");
        exit(-1);
    }
}

bool Parser::isAtEnd(){
    return this->pos_ >= this->toks_.size();
}