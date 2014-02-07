/* -*- Mode:C++; c-file-style:"gnu"; indent-tabs-mode:nil -*- */
/**
 * Copyright (C) 2013 Regents of the University of California.
 * @author: Yingdi Yu <yingdi@cs.ucla.edu>
 * @author: Jeff Thompson <jefft0@remap.ucla.edu>
 * See COPYING for copyright and distribution information.
 */

#include "validator.hpp"
#include "../util/logging.hpp"

#include <cryptopp/rsa.h>

using namespace std;

INIT_LOGGER("ndn::Validator");

namespace ndn {

const shared_ptr<Face> Validator::DefaultFace = shared_ptr<Face>();

Validator::Validator(shared_ptr<Face> face /* = DefaultFace */)                   
  : m_face(face)
{}

void
Validator::validate(const shared_ptr<const Interest> &interest, 
                    const OnInterestValidated &onValidated, 
                    const OnInterestValidationFailed &onValidationFailed,
                    int stepCount)
{
  vector<shared_ptr<ValidationRequest> > nextSteps;
  checkPolicy(interest, stepCount, onValidated, onValidationFailed, nextSteps);
  
  if (!nextSteps.empty())
    {
      if(!static_cast<bool>(m_face))
        throw Error("Face should be set prior to verify method to call");
      
      vector<shared_ptr<ValidationRequest> >::const_iterator it = nextSteps.begin();
      OnFailure onFailure = bind(onValidationFailed, interest);
      for(; it != nextSteps.end(); it++)
        m_face->expressInterest((*it)->m_interest,
                                bind(&Validator::onData, this, _1, _2, *it), 
                                bind(&Validator::onTimeout, 
                                     this, _1, (*it)->m_retry, 
                                     onFailure, 
                                     *it));
    }
  else
    {
      //If there is no nextStep, that means InterestPolicy has already been able to verify the Interest.
      //No more further processes.
    }
}

void
Validator::validate(const shared_ptr<const Data> &data, 
                    const OnDataValidated &onValidated, 
                    const OnDataValidationFailed &onValidationFailed,
                    int stepCount)
{
  vector<shared_ptr<ValidationRequest> > nextSteps;
  checkPolicy(data, stepCount, onValidated, onValidationFailed, nextSteps);

  if (!nextSteps.empty())
    {
      if(!static_cast<bool>(m_face))
        throw Error("Face should be set prior to verify method to call");

      vector<shared_ptr<ValidationRequest> >::const_iterator it = nextSteps.begin();
      OnFailure onFailure = bind(onValidationFailed, data);
      for(; it != nextSteps.end(); it++)
        m_face->expressInterest((*it)->m_interest,
                                bind(&Validator::onData, this, _1, _2, *it), 
                                bind(&Validator::onTimeout, 
                                     this, _1, (*it)->m_retry, 
                                     onFailure,
                                     *it));
    }
  else
    {
      //If there is no nextStep, that means InterestPolicy has already been able to verify the Interest.
      //No more further processes.
    }
}

void
Validator::onData(const shared_ptr<const Interest> &interest, 
                  const shared_ptr<const Data> &data, 
                  shared_ptr<ValidationRequest> nextStep)
{
  validate(data, nextStep->m_onValidated, nextStep->m_onDataValidated, nextStep->m_stepCount);
}

void
Validator::onTimeout(const shared_ptr<const Interest> &interest, 
                     int retry, 
                     const OnFailure &onFailure, 
                     shared_ptr<ValidationRequest> nextStep)
{
  if (retry > 0)
    // Issue the same expressInterest except decrement retry.
    m_face->expressInterest(*interest, 
                            bind(&Validator::onData, this, _1, _2, nextStep), 
                            bind(&Validator::onTimeout, this, _1, retry - 1, onFailure, nextStep));
  else
    onFailure();
}

bool
Validator::verifySignature(const Data& data, const PublicKey& key)
{
  try{
    switch(data.getSignature().getType()){
    case Signature::Sha256WithRsa:
      {
        SignatureSha256WithRsa sigSha256Rsa(data.getSignature());
        return verifySignature(data, sigSha256Rsa, key);
      }
    default:
      {
        _LOG_DEBUG("verifySignature: Unknown signature type: " << sig.getType());
        return false;
      }
    }
  }catch(Signature::Error &e){
    _LOG_DEBUG("verifySignature: " << e.what());
    return false;
  }
  return false;
}

bool
Validator::verifySignature(const Interest &interest, const PublicKey &key)
{
  const Name &interestName = interest.getName();

  if(interestName.size() < 3)
    return false;

  try{
    const Block &nameBlock = interestName.wireEncode();

    if(nameBlock.getAll().size() != interestName.size()) //HACK!! we should change it when Name::Component is changed to derive from Block.
      const_cast<Block&>(nameBlock).parse();

    Signature sig((++nameBlock.getAll().rbegin())->blockFromValue(), 
                  (nameBlock.getAll().rbegin())->blockFromValue());

    switch(sig.getType()){
    case Signature::Sha256WithRsa:
      {
        SignatureSha256WithRsa sigSha256Rsa(sig);

        return verifySignature(nameBlock.value(), 
                               nameBlock.value_size() - (nameBlock.getAll().rbegin())->size(), 
                               sigSha256Rsa, key);
      }
    default:
      {
        _LOG_DEBUG("verifySignature: Unknown signature type: " << sig.getType());
        return false;
      }
    }
  }catch(Signature::Error &e){
    _LOG_DEBUG("verifySignature: " << e.what());
    return false;
  }catch(Block::Error &e){
    _LOG_DEBUG("verifySignature: " << e.what());
    return false;
  }
  return false;
}

bool
Validator::verifySignature(const Buffer &data, const Signature &sig, const PublicKey &key)
{
  try{
    switch(sig.getType()){
    case Signature::Sha256WithRsa:
      {
        SignatureSha256WithRsa sigSha256Rsa(sig);
        return verifySignature(data, sigSha256Rsa, key);
      }
    default:
      {
        _LOG_DEBUG("verifySignature: Unknown signature type: " << sig.getType());
        return false;
      }
    }
  }catch(Signature::Error &e){
    _LOG_DEBUG("verifySignature: " << e.what());
    return false;
  }
  return false;
}

bool
Validator::verifySignature(const Data& data, const SignatureSha256WithRsa& sig, const PublicKey& key)
{
  using namespace CryptoPP;

  bool result = false;
  
  RSA::PublicKey publicKey;
  ByteQueue queue;

  queue.Put(reinterpret_cast<const byte*>(key.get().buf()), key.get().size());
  publicKey.Load(queue);

  RSASS<PKCS1v15, SHA256>::Verifier verifier (publicKey);
  result = verifier.VerifyMessage(data.wireEncode().value(), data.wireEncode().value_size() - data.getSignature().getValue().size(),
				  sig.getValue().value(), sig.getValue().value_size());

  _LOG_DEBUG("Signature verified? " << data.getName().toUri() << " " << boolalpha << result);
  
  return result;
}

bool
Validator::verifySignature(const Buffer& data, const SignatureSha256WithRsa& sig, const PublicKey& key)
{
  using namespace CryptoPP;

  bool result = false;
  
  RSA::PublicKey publicKey;
  ByteQueue queue;

  queue.Put(reinterpret_cast<const byte*>(key.get().buf()), key.get().size());
  publicKey.Load(queue);

  RSASS<PKCS1v15, SHA256>::Verifier verifier (publicKey);
  result = verifier.VerifyMessage(data.buf(), data.size(),
				  sig.getValue().value(), sig.getValue().value_size());
  
  return result;
}

bool
Validator::verifySignature(const uint8_t* buf, const size_t size, const SignatureSha256WithRsa &sig, const PublicKey &key)
{
  using namespace CryptoPP;

  bool result = false;
  
  RSA::PublicKey publicKey;
  ByteQueue queue;

  queue.Put(reinterpret_cast<const byte*>(key.get().buf()), key.get().size());
  publicKey.Load(queue);

  RSASS<PKCS1v15, SHA256>::Verifier verifier (publicKey);
  result = verifier.VerifyMessage(buf, size, sig.getValue().value(), sig.getValue().value_size());
  
  return result;
}

}