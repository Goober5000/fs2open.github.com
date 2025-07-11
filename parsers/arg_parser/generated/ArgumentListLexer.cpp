
// Generated from ArgumentList.g4 by ANTLR 4.13.2


#include "ArgumentListLexer.h"


using namespace antlr4;



using namespace antlr4;

namespace {

struct ArgumentListLexerStaticData final {
  ArgumentListLexerStaticData(std::vector<std::string> ruleNames,
                          std::vector<std::string> channelNames,
                          std::vector<std::string> modeNames,
                          std::vector<std::string> literalNames,
                          std::vector<std::string> symbolicNames)
      : ruleNames(std::move(ruleNames)), channelNames(std::move(channelNames)),
        modeNames(std::move(modeNames)), literalNames(std::move(literalNames)),
        symbolicNames(std::move(symbolicNames)),
        vocabulary(this->literalNames, this->symbolicNames) {}

  ArgumentListLexerStaticData(const ArgumentListLexerStaticData&) = delete;
  ArgumentListLexerStaticData(ArgumentListLexerStaticData&&) = delete;
  ArgumentListLexerStaticData& operator=(const ArgumentListLexerStaticData&) = delete;
  ArgumentListLexerStaticData& operator=(ArgumentListLexerStaticData&&) = delete;

  std::vector<antlr4::dfa::DFA> decisionToDFA;
  antlr4::atn::PredictionContextCache sharedContextCache;
  const std::vector<std::string> ruleNames;
  const std::vector<std::string> channelNames;
  const std::vector<std::string> modeNames;
  const std::vector<std::string> literalNames;
  const std::vector<std::string> symbolicNames;
  const antlr4::dfa::Vocabulary vocabulary;
  antlr4::atn::SerializedATNView serializedATN;
  std::unique_ptr<antlr4::atn::ATN> atn;
};

::antlr4::internal::OnceFlag argumentlistlexerLexerOnceFlag;
#if ANTLR4_USE_THREAD_LOCAL_CACHE
static thread_local
#endif
std::unique_ptr<ArgumentListLexerStaticData> argumentlistlexerLexerStaticData = nullptr;

void argumentlistlexerLexerInitialize() {
#if ANTLR4_USE_THREAD_LOCAL_CACHE
  if (argumentlistlexerLexerStaticData != nullptr) {
    return;
  }
#else
  assert(argumentlistlexerLexerStaticData == nullptr);
#endif
  auto staticData = std::make_unique<ArgumentListLexerStaticData>(
    std::vector<std::string>{
      "COMMA", "EQUALS", "STRING", "NIL", "TRUE", "FALSE", "FUNCTION", "VARARGS_SPECIFIER", 
      "NUMBER", "TYPE_ALT", "L_BRACKET", "R_BRACKET", "L_PAREN", "R_PAREN", 
      "L_CURLY", "R_CURLY", "ARROW", "ITERATOR", "L_ANGLE_BRACKET", "R_ANGLE_BRACKET", 
      "ARG_COMMENT", "ID", "SPACE", "OTHER"
    },
    std::vector<std::string>{
      "DEFAULT_TOKEN_CHANNEL", "HIDDEN"
    },
    std::vector<std::string>{
      "DEFAULT_MODE"
    },
    std::vector<std::string>{
      "", "','", "'='", "", "'nil'", "'true'", "'false'", "'function'", 
      "'...'", "", "", "'['", "']'", "'('", "')'", "'{'", "'}'", "'=>'", 
      "'iterator'", "'<'", "'>'"
    },
    std::vector<std::string>{
      "", "COMMA", "EQUALS", "STRING", "NIL", "TRUE", "FALSE", "FUNCTION", 
      "VARARGS_SPECIFIER", "NUMBER", "TYPE_ALT", "L_BRACKET", "R_BRACKET", 
      "L_PAREN", "R_PAREN", "L_CURLY", "R_CURLY", "ARROW", "ITERATOR", "L_ANGLE_BRACKET", 
      "R_ANGLE_BRACKET", "ARG_COMMENT", "ID", "SPACE", "OTHER"
    }
  );
  static const int32_t serializedATNSegment[] = {
  	4,0,24,182,6,-1,2,0,7,0,2,1,7,1,2,2,7,2,2,3,7,3,2,4,7,4,2,5,7,5,2,6,7,
  	6,2,7,7,7,2,8,7,8,2,9,7,9,2,10,7,10,2,11,7,11,2,12,7,12,2,13,7,13,2,14,
  	7,14,2,15,7,15,2,16,7,16,2,17,7,17,2,18,7,18,2,19,7,19,2,20,7,20,2,21,
  	7,21,2,22,7,22,2,23,7,23,1,0,1,0,1,1,1,1,1,2,1,2,5,2,56,8,2,10,2,12,2,
  	59,9,2,1,2,1,2,1,2,5,2,64,8,2,10,2,12,2,67,9,2,1,2,3,2,70,8,2,1,3,1,3,
  	1,3,1,3,1,4,1,4,1,4,1,4,1,4,1,5,1,5,1,5,1,5,1,5,1,5,1,6,1,6,1,6,1,6,1,
  	6,1,6,1,6,1,6,1,6,1,7,1,7,1,7,1,7,1,8,3,8,101,8,8,1,8,4,8,104,8,8,11,
  	8,12,8,105,1,8,4,8,109,8,8,11,8,12,8,110,1,8,1,8,5,8,115,8,8,10,8,12,
  	8,118,9,8,1,8,1,8,4,8,122,8,8,11,8,12,8,123,3,8,126,8,8,1,9,1,9,1,10,
  	1,10,1,11,1,11,1,12,1,12,1,13,1,13,1,14,1,14,1,15,1,15,1,16,1,16,1,16,
  	1,17,1,17,1,17,1,17,1,17,1,17,1,17,1,17,1,17,1,18,1,18,1,19,1,19,1,20,
  	1,20,1,20,1,20,5,20,162,8,20,10,20,12,20,165,9,20,1,20,1,20,1,20,1,21,
  	1,21,5,21,172,8,21,10,21,12,21,175,9,21,1,22,1,22,1,22,1,22,1,23,1,23,
  	1,163,0,24,1,1,3,2,5,3,7,4,9,5,11,6,13,7,15,8,17,9,19,10,21,11,23,12,
  	25,13,27,14,29,15,31,16,33,17,35,18,37,19,39,20,41,21,43,22,45,23,47,
  	24,1,0,7,3,0,10,10,13,13,34,34,3,0,10,10,13,13,39,39,1,0,48,57,2,0,47,
  	47,124,124,3,0,65,90,95,95,97,122,4,0,48,58,65,90,95,95,97,122,3,0,9,
  	10,13,13,32,32,193,0,1,1,0,0,0,0,3,1,0,0,0,0,5,1,0,0,0,0,7,1,0,0,0,0,
  	9,1,0,0,0,0,11,1,0,0,0,0,13,1,0,0,0,0,15,1,0,0,0,0,17,1,0,0,0,0,19,1,
  	0,0,0,0,21,1,0,0,0,0,23,1,0,0,0,0,25,1,0,0,0,0,27,1,0,0,0,0,29,1,0,0,
  	0,0,31,1,0,0,0,0,33,1,0,0,0,0,35,1,0,0,0,0,37,1,0,0,0,0,39,1,0,0,0,0,
  	41,1,0,0,0,0,43,1,0,0,0,0,45,1,0,0,0,0,47,1,0,0,0,1,49,1,0,0,0,3,51,1,
  	0,0,0,5,69,1,0,0,0,7,71,1,0,0,0,9,75,1,0,0,0,11,80,1,0,0,0,13,86,1,0,
  	0,0,15,95,1,0,0,0,17,100,1,0,0,0,19,127,1,0,0,0,21,129,1,0,0,0,23,131,
  	1,0,0,0,25,133,1,0,0,0,27,135,1,0,0,0,29,137,1,0,0,0,31,139,1,0,0,0,33,
  	141,1,0,0,0,35,144,1,0,0,0,37,153,1,0,0,0,39,155,1,0,0,0,41,157,1,0,0,
  	0,43,169,1,0,0,0,45,176,1,0,0,0,47,180,1,0,0,0,49,50,5,44,0,0,50,2,1,
  	0,0,0,51,52,5,61,0,0,52,4,1,0,0,0,53,57,5,34,0,0,54,56,8,0,0,0,55,54,
  	1,0,0,0,56,59,1,0,0,0,57,55,1,0,0,0,57,58,1,0,0,0,58,60,1,0,0,0,59,57,
  	1,0,0,0,60,70,5,34,0,0,61,65,5,39,0,0,62,64,8,1,0,0,63,62,1,0,0,0,64,
  	67,1,0,0,0,65,63,1,0,0,0,65,66,1,0,0,0,66,68,1,0,0,0,67,65,1,0,0,0,68,
  	70,5,39,0,0,69,53,1,0,0,0,69,61,1,0,0,0,70,6,1,0,0,0,71,72,5,110,0,0,
  	72,73,5,105,0,0,73,74,5,108,0,0,74,8,1,0,0,0,75,76,5,116,0,0,76,77,5,
  	114,0,0,77,78,5,117,0,0,78,79,5,101,0,0,79,10,1,0,0,0,80,81,5,102,0,0,
  	81,82,5,97,0,0,82,83,5,108,0,0,83,84,5,115,0,0,84,85,5,101,0,0,85,12,
  	1,0,0,0,86,87,5,102,0,0,87,88,5,117,0,0,88,89,5,110,0,0,89,90,5,99,0,
  	0,90,91,5,116,0,0,91,92,5,105,0,0,92,93,5,111,0,0,93,94,5,110,0,0,94,
  	14,1,0,0,0,95,96,5,46,0,0,96,97,5,46,0,0,97,98,5,46,0,0,98,16,1,0,0,0,
  	99,101,5,45,0,0,100,99,1,0,0,0,100,101,1,0,0,0,101,125,1,0,0,0,102,104,
  	7,2,0,0,103,102,1,0,0,0,104,105,1,0,0,0,105,103,1,0,0,0,105,106,1,0,0,
  	0,106,126,1,0,0,0,107,109,7,2,0,0,108,107,1,0,0,0,109,110,1,0,0,0,110,
  	108,1,0,0,0,110,111,1,0,0,0,111,112,1,0,0,0,112,116,5,46,0,0,113,115,
  	7,2,0,0,114,113,1,0,0,0,115,118,1,0,0,0,116,114,1,0,0,0,116,117,1,0,0,
  	0,117,126,1,0,0,0,118,116,1,0,0,0,119,121,5,46,0,0,120,122,7,2,0,0,121,
  	120,1,0,0,0,122,123,1,0,0,0,123,121,1,0,0,0,123,124,1,0,0,0,124,126,1,
  	0,0,0,125,103,1,0,0,0,125,108,1,0,0,0,125,119,1,0,0,0,126,18,1,0,0,0,
  	127,128,7,3,0,0,128,20,1,0,0,0,129,130,5,91,0,0,130,22,1,0,0,0,131,132,
  	5,93,0,0,132,24,1,0,0,0,133,134,5,40,0,0,134,26,1,0,0,0,135,136,5,41,
  	0,0,136,28,1,0,0,0,137,138,5,123,0,0,138,30,1,0,0,0,139,140,5,125,0,0,
  	140,32,1,0,0,0,141,142,5,61,0,0,142,143,5,62,0,0,143,34,1,0,0,0,144,145,
  	5,105,0,0,145,146,5,116,0,0,146,147,5,101,0,0,147,148,5,114,0,0,148,149,
  	5,97,0,0,149,150,5,116,0,0,150,151,5,111,0,0,151,152,5,114,0,0,152,36,
  	1,0,0,0,153,154,5,60,0,0,154,38,1,0,0,0,155,156,5,62,0,0,156,40,1,0,0,
  	0,157,158,5,47,0,0,158,159,5,42,0,0,159,163,1,0,0,0,160,162,9,0,0,0,161,
  	160,1,0,0,0,162,165,1,0,0,0,163,164,1,0,0,0,163,161,1,0,0,0,164,166,1,
  	0,0,0,165,163,1,0,0,0,166,167,5,42,0,0,167,168,5,47,0,0,168,42,1,0,0,
  	0,169,173,7,4,0,0,170,172,7,5,0,0,171,170,1,0,0,0,172,175,1,0,0,0,173,
  	171,1,0,0,0,173,174,1,0,0,0,174,44,1,0,0,0,175,173,1,0,0,0,176,177,7,
  	6,0,0,177,178,1,0,0,0,178,179,6,22,0,0,179,46,1,0,0,0,180,181,9,0,0,0,
  	181,48,1,0,0,0,12,0,57,65,69,100,105,110,116,123,125,163,173,1,6,0,0
  };
  staticData->serializedATN = antlr4::atn::SerializedATNView(serializedATNSegment, sizeof(serializedATNSegment) / sizeof(serializedATNSegment[0]));

  antlr4::atn::ATNDeserializer deserializer;
  staticData->atn = deserializer.deserialize(staticData->serializedATN);

  const size_t count = staticData->atn->getNumberOfDecisions();
  staticData->decisionToDFA.reserve(count);
  for (size_t i = 0; i < count; i++) { 
    staticData->decisionToDFA.emplace_back(staticData->atn->getDecisionState(i), i);
  }
  argumentlistlexerLexerStaticData = std::move(staticData);
}

}

ArgumentListLexer::ArgumentListLexer(CharStream *input) : Lexer(input) {
  ArgumentListLexer::initialize();
  _interpreter = new atn::LexerATNSimulator(this, *argumentlistlexerLexerStaticData->atn, argumentlistlexerLexerStaticData->decisionToDFA, argumentlistlexerLexerStaticData->sharedContextCache);
}

ArgumentListLexer::~ArgumentListLexer() {
  delete _interpreter;
}

std::string ArgumentListLexer::getGrammarFileName() const {
  return "ArgumentList.g4";
}

const std::vector<std::string>& ArgumentListLexer::getRuleNames() const {
  return argumentlistlexerLexerStaticData->ruleNames;
}

const std::vector<std::string>& ArgumentListLexer::getChannelNames() const {
  return argumentlistlexerLexerStaticData->channelNames;
}

const std::vector<std::string>& ArgumentListLexer::getModeNames() const {
  return argumentlistlexerLexerStaticData->modeNames;
}

const dfa::Vocabulary& ArgumentListLexer::getVocabulary() const {
  return argumentlistlexerLexerStaticData->vocabulary;
}

antlr4::atn::SerializedATNView ArgumentListLexer::getSerializedATN() const {
  return argumentlistlexerLexerStaticData->serializedATN;
}

const atn::ATN& ArgumentListLexer::getATN() const {
  return *argumentlistlexerLexerStaticData->atn;
}




void ArgumentListLexer::initialize() {
#if ANTLR4_USE_THREAD_LOCAL_CACHE
  argumentlistlexerLexerInitialize();
#else
  ::antlr4::internal::call_once(argumentlistlexerLexerOnceFlag, argumentlistlexerLexerInitialize);
#endif
}
