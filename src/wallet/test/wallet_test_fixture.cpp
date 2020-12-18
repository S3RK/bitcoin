// Copyright (c) 2016-2020 The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <util/translation.h>
#include <wallet/test/wallet_test_fixture.h>

WalletTestingSetup::WalletTestingSetup(const std::string& chainName)
    : TestingSetup(chainName),
      m_wallet("", CreateMockWalletDatabase())
{
    m_wallet.LoadWallet();
    bilingual_str error;
    std::vector<bilingual_str> warnings;
    m_wallet.AttachChain(*m_chain.get(), error, warnings);
    m_wallet_client->registerRpcs();
}
