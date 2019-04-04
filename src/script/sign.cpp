// Copyright (c) 2009-2010 Satoshi Nakamoto
// Copyright (c) 2009-2016 The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <script/sign.h>

#include <key.h>
#include <policy/policy.h>
#include <primitives/transaction.h>
#include <script/standard.h>
#include <uint256.h>

typedef std::vector<uint8_t> valtype;

MutableTransactionSignatureCreator::MutableTransactionSignatureCreator(
    const CMutableTransaction *txToIn, unsigned int nInIn,
    const Amount &amountIn, SigHashType sigHashTypeIn)
    : txTo(txToIn), nIn(nInIn), amount(amountIn), sigHashType(sigHashTypeIn),
      checker(txTo, nIn, amountIn) {}

bool MutableTransactionSignatureCreator::CreateSig(
    const SigningProvider &provider, std::vector<uint8_t> &vchSig,
    const CKeyID &address, const CScript &scriptCode) const {
    CKey key;
    if (!provider.GetKey(address, key)) {
        return false;
    }

    uint256 hash = SignatureHash(scriptCode, *txTo, nIn, sigHashType, amount);
    if (!key.SignECDSA(hash, vchSig)) {
        return false;
    }

    vchSig.push_back(uint8_t(sigHashType.getRawSigHashType()));
    return true;
}

static bool GetCScript(const SigningProvider &provider,
                       const SignatureData &sigdata, const CScriptID &scriptid,
                       CScript &script) {
    if (provider.GetCScript(scriptid, script)) {
        return true;
    }
    // Look for scripts in SignatureData
    if (CScriptID(sigdata.redeem_script) == scriptid) {
        script = sigdata.redeem_script;
        return true;
    }
    return false;
}

static bool GetPubKey(const SigningProvider &provider, SignatureData &sigdata,
                      const CKeyID &address, CPubKey &pubkey) {
    // Look for pubkey in all partial sigs
    const auto it = sigdata.signatures.find(address);
    if (it != sigdata.signatures.end()) {
        pubkey = it->second.first;
        return true;
    }
    // Look for pubkey in pubkey list
    const auto &pk_it = sigdata.misc_pubkeys.find(address);
    if (pk_it != sigdata.misc_pubkeys.end()) {
        pubkey = pk_it->second.first;
        return true;
    }
    // Query the underlying provider
    if (provider.GetPubKey(address, pubkey)) {
        KeyOriginInfo info;
        if (provider.GetKeyOrigin(address, info)) {
            sigdata.misc_pubkeys.emplace(
                address, std::make_pair(pubkey, std::move(info)));
        }
        return true;
    }
    return false;
}

static bool CreateSig(const BaseSignatureCreator &creator,
                      SignatureData &sigdata, const SigningProvider &provider,
                      std::vector<uint8_t> &sig_out, const CPubKey &pubkey,
                      const CScript &scriptcode) {
    CKeyID keyid = pubkey.GetID();
    const auto it = sigdata.signatures.find(keyid);
    if (it != sigdata.signatures.end()) {
        sig_out = it->second.second;
        return true;
    }
    KeyOriginInfo info;
    if (provider.GetKeyOrigin(keyid, info)) {
        sigdata.misc_pubkeys.emplace(keyid,
                                     std::make_pair(pubkey, std::move(info)));
    }
    if (creator.CreateSig(provider, sig_out, keyid, scriptcode)) {
        auto i = sigdata.signatures.emplace(keyid, SigPair(pubkey, sig_out));
        assert(i.second);
        return true;
    }
    // Could not make signature or signature not found, add keyid to missing
    sigdata.missing_sigs.push_back(keyid);
    return false;
}

/**
 * Sign scriptPubKey using signature made with creator.
 * Signatures are returned in scriptSigRet (or returns false if scriptPubKey
 * can't be signed), unless whichTypeRet is TX_SCRIPTHASH, in which case
 * scriptSigRet is the redemption script.
 * Returns false if scriptPubKey could not be completely satisfied.
 */
static bool SignStep(const SigningProvider &provider,
                     const BaseSignatureCreator &creator,
                     const CScript &scriptPubKey, std::vector<valtype> &ret,
                     txnouttype &whichTypeRet, SignatureData &sigdata) {
    CScript scriptRet;
    uint160 h160;
    ret.clear();
    std::vector<uint8_t> sig;

    std::vector<valtype> vSolutions;
    whichTypeRet = Solver(scriptPubKey, vSolutions);

    switch (whichTypeRet) {
        case TX_NONSTANDARD:
        case TX_NULL_DATA:
            return false;
        case TX_PUBKEY:
            if (!CreateSig(creator, sigdata, provider, sig,
                           CPubKey(vSolutions[0]), scriptPubKey)) {
                return false;
            }
            ret.push_back(std::move(sig));
            return true;
        case TX_PUBKEYHASH: {
            CKeyID keyID = CKeyID(uint160(vSolutions[0]));
            CPubKey pubkey;
            if (!GetPubKey(provider, sigdata, keyID, pubkey)) {
                // Pubkey could not be found, add to missing
                sigdata.missing_pubkeys.push_back(keyID);
                return false;
            }
            if (!CreateSig(creator, sigdata, provider, sig, pubkey,
                           scriptPubKey)) {
                return false;
            }
            ret.push_back(std::move(sig));
            ret.push_back(ToByteVector(pubkey));
            return true;
        }
        case TX_SCRIPTHASH:
            h160 = uint160(vSolutions[0]);
            if (GetCScript(provider, sigdata, h160, scriptRet)) {
                ret.push_back(
                    std::vector<uint8_t>(scriptRet.begin(), scriptRet.end()));
                return true;
            }
            // Could not find redeemScript, add to missing
            sigdata.missing_redeem_script = h160;
            return false;
        case TX_MULTISIG: {
            size_t required = vSolutions.front()[0];
            // workaround CHECKMULTISIG bug
            ret.push_back(valtype());
            for (size_t i = 1; i < vSolutions.size() - 1; ++i) {
                CPubKey pubkey = CPubKey(vSolutions[i]);
                if (ret.size() < required + 1 &&
                    CreateSig(creator, sigdata, provider, sig, pubkey,
                              scriptPubKey)) {
                    ret.push_back(std::move(sig));
                }
            }
            bool ok = ret.size() == required + 1;
            for (size_t i = 0; i + ret.size() < required + 1; ++i) {
                ret.push_back(valtype());
            }
            return ok;
        }
        default:
            return false;
    }
}

static CScript PushAll(const std::vector<valtype> &values) {
    CScript result;
    for (const valtype &v : values) {
        if (v.size() == 0) {
            result << OP_0;
        } else if (v.size() == 1 && v[0] >= 1 && v[0] <= 16) {
            result << CScript::EncodeOP_N(v[0]);
        } else {
            result << v;
        }
    }

    return result;
}

bool ProduceSignature(const SigningProvider &provider,
                      const BaseSignatureCreator &creator,
                      const CScript &fromPubKey, SignatureData &sigdata) {
    if (sigdata.complete) {
        return true;
    }

    std::vector<valtype> result;
    txnouttype whichType;
    bool solved =
        SignStep(provider, creator, fromPubKey, result, whichType, sigdata);
    CScript subscript;

    if (solved && whichType == TX_SCRIPTHASH) {
        // Solver returns the subscript that needs to be evaluated; the final
        // scriptSig is the signatures from that and then the serialized
        // subscript:
        subscript = CScript(result[0].begin(), result[0].end());
        sigdata.redeem_script = subscript;

        solved = solved &&
                 SignStep(provider, creator, subscript, result, whichType,
                          sigdata) &&
                 whichType != TX_SCRIPTHASH;
        result.push_back(
            std::vector<uint8_t>(subscript.begin(), subscript.end()));
    }

    sigdata.scriptSig = PushAll(result);

    // Test solution
    sigdata.complete =
        solved && VerifyScript(sigdata.scriptSig, fromPubKey,
                               STANDARD_SCRIPT_VERIFY_FLAGS, creator.Checker());
    return sigdata.complete;
}

class SignatureExtractorChecker final : public BaseSignatureChecker {
private:
    SignatureData &sigdata;
    BaseSignatureChecker &checker;

public:
    SignatureExtractorChecker(SignatureData &sigdata_,
                              BaseSignatureChecker &checker_)
        : sigdata(sigdata_), checker(checker_) {}
    bool CheckSig(const std::vector<uint8_t> &scriptSig,
                  const std::vector<uint8_t> &vchPubKey,
                  const CScript &scriptCode, uint32_t flags) const override;
};

bool SignatureExtractorChecker::CheckSig(const std::vector<uint8_t> &scriptSig,
                                         const std::vector<uint8_t> &vchPubKey,
                                         const CScript &scriptCode,
                                         uint32_t flags) const {
    if (checker.CheckSig(scriptSig, vchPubKey, scriptCode, flags)) {
        CPubKey pubkey(vchPubKey);
        sigdata.signatures.emplace(pubkey.GetID(), SigPair(pubkey, scriptSig));
        return true;
    }
    return false;
}

namespace {
struct Stacks {
    std::vector<valtype> script;

    Stacks() = delete;
    Stacks(const Stacks &) = delete;
    explicit Stacks(const SignatureData &data) {
        if (data.scriptSig.IsPushOnly()) {
            EvalScript(script, data.scriptSig, SCRIPT_VERIFY_NONE,
                       BaseSignatureChecker());
        }
    }
};
} // namespace

// Extracts signatures and scripts from incomplete scriptSigs. Please do not
// extend this, use PSBT instead
SignatureData DataFromTransaction(const CMutableTransaction &tx,
                                  unsigned int nIn, const CTxOut &txout) {
    SignatureData data;
    assert(tx.vin.size() > nIn);
    data.scriptSig = tx.vin[nIn].scriptSig;
    Stacks stack(data);

    // Get signatures
    MutableTransactionSignatureChecker tx_checker(&tx, nIn, txout.nValue);
    SignatureExtractorChecker extractor_checker(data, tx_checker);
    if (VerifyScript(data.scriptSig, txout.scriptPubKey,
                     STANDARD_SCRIPT_VERIFY_FLAGS, extractor_checker)) {
        data.complete = true;
        return data;
    }

    // Get scripts
    std::vector<std::vector<uint8_t>> solutions;
    txnouttype script_type = Solver(txout.scriptPubKey, solutions);
    CScript next_script = txout.scriptPubKey;

    if (script_type == TX_SCRIPTHASH && !stack.script.empty() &&
        !stack.script.back().empty()) {
        // Get the redeemScript
        CScript redeem_script(stack.script.back().begin(),
                              stack.script.back().end());
        data.redeem_script = redeem_script;
        next_script = std::move(redeem_script);

        // Get redeemScript type
        script_type = Solver(next_script, solutions);
        stack.script.pop_back();
    }
    if (script_type == TX_MULTISIG && !stack.script.empty()) {
        // Build a map of pubkey -> signature by matching sigs to pubkeys:
        assert(solutions.size() > 1);
        unsigned int num_pubkeys = solutions.size() - 2;
        unsigned int last_success_key = 0;
        for (const valtype &sig : stack.script) {
            for (unsigned int i = last_success_key; i < num_pubkeys; ++i) {
                const valtype &pubkey = solutions[i + 1];
                // We either have a signature for this pubkey, or we have found
                // a signature and it is valid
                if (data.signatures.count(CPubKey(pubkey).GetID()) ||
                    extractor_checker.CheckSig(sig, pubkey, next_script,
                                               STANDARD_SCRIPT_VERIFY_FLAGS)) {
                    last_success_key = i + 1;
                    break;
                }
            }
        }
    }

    return data;
}

void UpdateInput(CTxIn &input, const SignatureData &data) {
    input.scriptSig = data.scriptSig;
}

void SignatureData::MergeSignatureData(SignatureData sigdata) {
    if (complete) {
        return;
    }
    if (sigdata.complete) {
        *this = std::move(sigdata);
        return;
    }
    if (redeem_script.empty() && !sigdata.redeem_script.empty()) {
        redeem_script = sigdata.redeem_script;
    }
    signatures.insert(std::make_move_iterator(sigdata.signatures.begin()),
                      std::make_move_iterator(sigdata.signatures.end()));
}

bool SignSignature(const SigningProvider &provider, const CScript &fromPubKey,
                   CMutableTransaction &txTo, unsigned int nIn,
                   const Amount amount, SigHashType sigHashType) {
    assert(nIn < txTo.vin.size());

    MutableTransactionSignatureCreator creator(&txTo, nIn, amount, sigHashType);

    SignatureData sigdata;
    bool ret = ProduceSignature(provider, creator, fromPubKey, sigdata);
    UpdateInput(txTo.vin.at(nIn), sigdata);
    return ret;
}

bool SignSignature(const SigningProvider &provider, const CTransaction &txFrom,
                   CMutableTransaction &txTo, unsigned int nIn,
                   SigHashType sigHashType) {
    assert(nIn < txTo.vin.size());
    CTxIn &txin = txTo.vin[nIn];
    assert(txin.prevout.GetN() < txFrom.vout.size());
    const CTxOut &txout = txFrom.vout[txin.prevout.GetN()];

    return SignSignature(provider, txout.scriptPubKey, txTo, nIn, txout.nValue,
                         sigHashType);
}

namespace {
/** Dummy signature checker which accepts all signatures. */
class DummySignatureChecker final : public BaseSignatureChecker {
public:
    DummySignatureChecker() {}
    bool CheckSig(const std::vector<uint8_t> &scriptSig,
                  const std::vector<uint8_t> &vchPubKey,
                  const CScript &scriptCode, uint32_t flags) const override {
        return true;
    }
};
const DummySignatureChecker DUMMY_CHECKER;

class DummySignatureCreator final : public BaseSignatureCreator {
private:
    char m_r_len = 32;
    char m_s_len = 32;

public:
    DummySignatureCreator(char r_len, char s_len)
        : m_r_len(r_len), m_s_len(s_len) {}
    const BaseSignatureChecker &Checker() const override {
        return DUMMY_CHECKER;
    }
    bool CreateSig(const SigningProvider &provider,
                   std::vector<uint8_t> &vchSig, const CKeyID &keyid,
                   const CScript &scriptCode) const override {
        // Create a dummy signature that is a valid DER-encoding
        vchSig.assign(m_r_len + m_s_len + 7, '\000');
        vchSig[0] = 0x30;
        vchSig[1] = m_r_len + m_s_len + 4;
        vchSig[2] = 0x02;
        vchSig[3] = m_r_len;
        vchSig[4] = 0x01;
        vchSig[4 + m_r_len] = 0x02;
        vchSig[5 + m_r_len] = m_s_len;
        vchSig[6 + m_r_len] = 0x01;
        vchSig[6 + m_r_len + m_s_len] = SIGHASH_ALL | SIGHASH_FORKID;
        return true;
    }
};

template <typename M, typename K, typename V>
bool LookupHelper(const M &map, const K &key, V &value) {
    auto it = map.find(key);
    if (it != map.end()) {
        value = it->second;
        return true;
    }
    return false;
}

} // namespace

const BaseSignatureCreator &DUMMY_SIGNATURE_CREATOR =
    DummySignatureCreator(32, 32);
const BaseSignatureCreator &DUMMY_MAXIMUM_SIGNATURE_CREATOR =
    DummySignatureCreator(33, 32);
const SigningProvider &DUMMY_SIGNING_PROVIDER = SigningProvider();

bool HidingSigningProvider::GetCScript(const CScriptID &scriptid,
                                       CScript &script) const {
    return m_provider->GetCScript(scriptid, script);
}

bool HidingSigningProvider::GetPubKey(const CKeyID &keyid,
                                      CPubKey &pubkey) const {
    return m_provider->GetPubKey(keyid, pubkey);
}

bool HidingSigningProvider::GetKey(const CKeyID &keyid, CKey &key) const {
    if (m_hide_secret) {
        return false;
    }
    return m_provider->GetKey(keyid, key);
}

bool HidingSigningProvider::GetKeyOrigin(const CKeyID &keyid,
                                         KeyOriginInfo &info) const {
    if (m_hide_origin) {
        return false;
    }
    return m_provider->GetKeyOrigin(keyid, info);
}

bool FlatSigningProvider::GetCScript(const CScriptID &scriptid,
                                     CScript &script) const {
    return LookupHelper(scripts, scriptid, script);
}
bool FlatSigningProvider::GetPubKey(const CKeyID &keyid,
                                    CPubKey &pubkey) const {
    return LookupHelper(pubkeys, keyid, pubkey);
}
bool FlatSigningProvider::GetKeyOrigin(const CKeyID &keyid,
                                       KeyOriginInfo &info) const {
    std::pair<CPubKey, KeyOriginInfo> out;
    bool ret = LookupHelper(origins, keyid, out);
    if (ret) {
        info = std::move(out.second);
    }
    return ret;
}
bool FlatSigningProvider::GetKey(const CKeyID &keyid, CKey &key) const {
    return LookupHelper(keys, keyid, key);
}

FlatSigningProvider Merge(const FlatSigningProvider &a,
                          const FlatSigningProvider &b) {
    FlatSigningProvider ret;
    ret.scripts = a.scripts;
    ret.scripts.insert(b.scripts.begin(), b.scripts.end());
    ret.pubkeys = a.pubkeys;
    ret.pubkeys.insert(b.pubkeys.begin(), b.pubkeys.end());
    ret.keys = a.keys;
    ret.keys.insert(b.keys.begin(), b.keys.end());
    ret.origins = a.origins;
    ret.origins.insert(b.origins.begin(), b.origins.end());
    return ret;
}

bool IsSolvable(const SigningProvider &provider, const CScript &script) {
    // This check is to make sure that the script we created can actually be
    // solved for and signed by us if we were to have the private keys. This is
    // just to make sure that the script is valid and that, if found in a
    // transaction, we would still accept and relay that transaction.
    SignatureData sigs;
    if (ProduceSignature(provider, DUMMY_SIGNATURE_CREATOR, script, sigs)) {
        // VerifyScript check is just defensive, and should never fail.
        bool verified =
            VerifyScript(sigs.scriptSig, script, STANDARD_SCRIPT_VERIFY_FLAGS,
                         DUMMY_CHECKER);
        assert(verified);
        return true;
    }
    return false;
}
