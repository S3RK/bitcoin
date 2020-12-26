// Copyright (c) 2016-2020 The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <wallet/test/wallet_test_fixture.h>

WalletTestingSetup::WalletTestingSetup(const std::string& chainName)
    : TestingSetup(chainName),
      m_wallet(m_chain.get(), "", CreateMockWalletDatabase())
{
    m_wallet.LoadWallet();
    CWallet::AttachChain({ &m_wallet, [](CWallet*) {} });
    m_wallet_client->registerRpcs();
}
