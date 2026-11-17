#include "playercontext.h"

PlayerContext::PlayerContext() {}

// 头文件里声明了析构但一直没定义（以前没人 delete 它所以没暴露）。
// Player 现在会 delete m_playerCtx，这里补上定义。
PlayerContext::~PlayerContext() = default;
