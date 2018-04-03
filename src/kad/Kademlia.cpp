/**
 *
 * MIT License
 * 
 * Copyright (c) 2018 drvcoin
 * 
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 * 
 * The above copyright notice and this permission notice shall be included in all
 * copies or substantial portions of the Software.
 * 
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 * 
 * =============================================================================
 */

#include <assert.h>
#include "protocol/Protocol.h"
#include "EventLoop.h"
#include "KBuckets.h"
#include "Thread.h"
#include "PackageDispatcher.h"
#include "FindNodeAction.h"
#include "FindValueAction.h"
#include "StoreAction.h"
#include "PingAction.h"
#include "Storage.h"
#include "Config.h"
#include "Kademlia.h"

namespace kad
{
  Kademlia::Kademlia()
    : kBuckets(nullptr)
  {
    this->kBuckets = std::unique_ptr<KBuckets>(new KBuckets(Config::NodeId()));

    this->thread = std::unique_ptr<Thread>(new Thread("Main"));

    this->dispatcher = std::unique_ptr<PackageDispatcher>(new PackageDispatcher(this->thread.get()));

    using namespace std::placeholders;

    this->dispatcher->SetRequestHandler(std::bind(&Kademlia::OnRequest, this, _1, _2));

    this->dispatcher->SetContactHandler(std::bind(&Kademlia::OnMessage, this, _1, _2));
  }

  Kademlia::~Kademlia()
  {
    this->SaveBuckets();
  }


  void Kademlia::Initialize()
  {
    THREAD_ENSURE(this->thread.get(), Initialize);

    if (!this->InitBuckets())
    {
      // TODO: error handling
      return;
    }

    // TODO: get bootstrp nodes

    std::vector<std::pair<KeyPtr, ContactPtr>> nodes;

    // this->kBuckets->FindClosestContacts(Config::NodeId(), nodes);
    this->kBuckets->GetAllContacts(nodes);

    if (nodes.empty())
    {
      // This is the initial root node
      this->ready = true;
      return;
    }

    auto result = AsyncResultPtr(new AsyncResult<std::vector<std::pair<KeyPtr, ContactPtr>>>());

    this->FindNode(Config::NodeId(), result,
      [this](AsyncResultPtr result)
      {
        auto rtn = dynamic_cast<AsyncResult<std::vector<std::pair<KeyPtr, ContactPtr>>> *>(result.get());
        if (rtn && rtn->GetResult().size() > 0)
        {
          this->OnInitPing(
            new std::vector<std::pair<KeyPtr, ContactPtr>>(std::move(rtn->GetResult())),
            new std::set<KeyPtr, KeyCompare>(),
            new std::set<KeyPtr, KeyCompare>()
          );
        }
        else
        {
          this->ready = true;
        }
      }
    );
  }


  void Kademlia::OnInitPing(const std::vector<std::pair<KeyPtr, ContactPtr>> * targets, std::set<KeyPtr, KeyCompare> * validating, std::set<KeyPtr, KeyCompare> * validated)
  {
    THREAD_ENSURE(this->thread.get(), OnInitPing, targets, validating, validated);

    for (const auto & node : (*targets))
    {
      if (validating->size() >= Config::Parallelism())
      {
        break;
      }

      if (validated->find(node.first) == validated->end() && validating->find(node.first) == validating->end())
      {
        auto result = AsyncResultPtr(new AsyncResult<bool>());

        auto key = node.first;

        validating->emplace(key);

        this->Ping(node.second, result,
          [this, key, targets, validating, validated](AsyncResultPtr result)
          {
            validating->erase(key);

            validated->emplace(key);

            auto rtn = dynamic_cast<AsyncResult<bool> *>(result.get());

            if (!rtn || !rtn->GetResult())
            {
              // Mark the node as unreachable
              this->kBuckets->EraseContact(key);
            }

            if (validated->size() < targets->size())
            {
              this->OnInitPing(targets, validating, validated);
            }
            else
            {
              delete targets;
              delete validating;
              delete validated;

              this->ready = true;
            }
          }
        );
      }
    }
  }

  void Kademlia::OnInitRefresh(size_t idx)
  {
    this->RefreshBucket(idx,
      [this, idx](AsyncResultPtr result)
      {
        auto rtn = dynamic_cast<AsyncResult<std::vector<std::pair<KeyPtr, ContactPtr>>> *>(result.get());

        if (idx < Key::KEY_LEN_BITS - 1 && rtn && rtn->GetResult().size() >= KBuckets::SizeK)
        {
          this->OnInitRefresh(idx + 1);
        }
        else
        {
          this->ready = true;
        }
      }
    );
  }


  void Kademlia::FindNode(KeyPtr target, AsyncResultPtr result, CompleteHandler handler, bool restrictBucket)
  {
    THREAD_ENSURE(this->thread.get(), FindNode, target, result, handler);

    std::vector<std::pair<KeyPtr, ContactPtr>> nodes;

    this->kBuckets->FindClosestContacts(target, nodes, restrictBucket);

    if (nodes.empty())
    {
      result->Complete();

      if (handler)
      {
        handler(result);
      }

      return;
    }

    auto action = std::unique_ptr<FindNodeAction>(new FindNodeAction(this->thread.get(), this->dispatcher.get()));

    action->Initialize(target, nodes);

    action->SetOnCompleteHandler(
      [this, result, handler](void * _sender, void * _args)
      {
        auto action = reinterpret_cast<FindNodeAction *>(_args);
        
        auto rtn = dynamic_cast<AsyncResult<std::vector<std::pair<KeyPtr, ContactPtr>>> *>(result.get());

        if (rtn)
        {
          std::vector<std::pair<KeyPtr, ContactPtr>> nodes;
          action->GetResult(nodes);
          rtn->Complete(std::move(nodes));
        }
        else if (result)
        {
          result->Complete();
        }

        if (handler)
        {
          handler(result);
        }
      },
      this
    );

    if (action->Start())
    {
      action.release();
    }
  }


  void Kademlia::FindValue(KeyPtr target, AsyncResultPtr result, CompleteHandler handler)
  {
    THREAD_ENSURE(this->thread.get(), FindValue, target, result, handler);

    if (!this->ready)
    {
      return;
    }

    std::vector<std::pair<KeyPtr, ContactPtr>> nodes;

    this->kBuckets->FindClosestContacts(target, nodes);

    auto action = std::unique_ptr<FindValueAction>(new FindValueAction(this->thread.get(), this->dispatcher.get()));

    action->Initialize(target, nodes);

    action->SetOnCompleteHandler(
      [this, result, handler](void * _sender, void * _args)
      {
        auto action = reinterpret_cast<FindValueAction *>(_args);

        auto rtn = dynamic_cast<AsyncResult<BufferPtr> *>(result.get());

        if (rtn)
        {
          rtn->Complete(action->GetResult());
        }
        else if (result)
        {
          result->Complete();
        }

        if (handler)
        {
          handler(result);
        }
      },
      this
    );

    if (action->Start())
    {
      action.release();
    }
  }


  void Kademlia::Store(KeyPtr hash, BufferPtr data, AsyncResultPtr result, CompleteHandler handler)
  {
    THREAD_ENSURE(this->thread.get(), Store, hash, data, result, handler);

    if (!this->ready)
    {
      return;
    }

    this->FindNode(hash, AsyncResultPtr(new AsyncResult<std::vector<std::pair<KeyPtr, ContactPtr>>>()),
      [this, hash, data, result, handler](AsyncResultPtr rtn)
      {
        const auto & nodes = AsyncResultHelper::GetResult<std::vector<std::pair<KeyPtr, ContactPtr>>>(rtn.get());

        auto complete = [result, handler](bool val)
        {
          auto storeResult = dynamic_cast<AsyncResult<bool> *>(result.get());
          if (storeResult)
          {
            storeResult->Complete(val);
          }
          else if (result)
          {
            result->Complete();
          }

          if (handler)
          {
            handler(result);
          }
        };

        if (nodes.empty())
        {
          complete(false);
          return;
        }

        auto action = std::unique_ptr<StoreAction>(new StoreAction(this->thread.get(), this->dispatcher.get()));

        action->Initialize(nodes, hash, data);

        action->SetOnCompleteHandler(
          [hash, complete](void * sender, void * args)
          {
            auto action = reinterpret_cast<StoreAction *>(args);

            complete(action->GetResult());
          },
          this
        );

        if (action->Start())
        {
          action.release();
        }
      }
    );
  }


  void Kademlia::Ping(ContactPtr target, AsyncResultPtr result, CompleteHandler handler)
  {
    THREAD_ENSURE(this->thread.get(), Ping, target, result, handler);

    if (!target)
    {
      return;
    }

    auto action = std::unique_ptr<PingAction>(new PingAction(this->thread.get(), this->dispatcher.get()));

    action->Initialize(target);

    action->SetOnCompleteHandler(
      [result, handler](void * _sender, void * _args)
      {
        auto action = reinterpret_cast<PingAction *>(_args);

        auto rtn = dynamic_cast<AsyncResult<bool> *>(result.get());

        if (rtn)
        {
          rtn->Complete(action->GetResult());
        }
        else if (result)
        {
          result->Complete();
        }

        if (handler)
        {
          handler(result);
        }
      },
      this
    );

    if (action->Start())
    {
      action.release();
    }
  }


  void Kademlia::Ping(KeyPtr target, AsyncResultPtr result, CompleteHandler handler)
  {
    THREAD_ENSURE(this->thread.get(), Ping, target, result, handler);

    if (!target)
    {
      return;
    }

    auto contact = this->kBuckets->FindContact(target);

    if (!contact)
    {
      auto rtn = dynamic_cast<AsyncResult<bool> *>(result.get());

      if (rtn)
      {
        rtn->Complete(false);
      }
      else if (result)
      {
        result->Complete();
      }

      if (handler)
      {
        handler(result);
      }
    }
    else
    {
      this->Ping(contact, result, handler);
    }
  }


  void Kademlia::RefreshBucket(size_t idx, CompleteHandler handler)
  {
    THREAD_ENSURE(this->thread.get(), RefreshBucket, idx, handler);

    assert(idx < Key::KEY_LEN_BITS);

    Key mask = {};

    mask.SetBit(idx, true);

    // a ^ b = c => a ^ c = b

    KeyPtr target = std::make_shared<Key>(Config::NodeId()->GetDistance(mask));

    auto result = AsyncResultPtr(new AsyncResult<std::vector<std::pair<KeyPtr, ContactPtr>>>());

    this->FindNode(target, result, handler, true);
  }


  void Kademlia::OnMessage(KeyPtr fromKey, ContactPtr fromContact)
  {
    THREAD_ENSURE(this->thread.get(), OnMessage, fromKey, fromContact);

    ContactPtr origin = this->kBuckets->UpdateContact(fromKey);

    if (origin)
    {
      *origin = *fromContact;
    }
    else
    {
      if (this->kBuckets->GetBucketSize(fromKey) < KBuckets::SizeK)
      {
        this->kBuckets->AddContact(fromKey, fromContact);
      }
      else
      {
        std::vector<std::pair<KeyPtr, ContactPtr>> nodes;
        this->kBuckets->GetOldContacts(fromKey, 1, nodes);

        auto oldKey = nodes[0].first;
        auto result = AsyncResultPtr(new AsyncResult<bool>());

        this->Ping(nodes[0].second, result,
          [this, oldKey, fromKey, fromContact, result](AsyncResultPtr)
          {
            if (!AsyncResultHelper::GetResult<bool>(result.get()))
            {
              this->kBuckets->EraseContact(oldKey);
              this->kBuckets->AddContact(fromKey, fromContact);
            }
          }
        );
      }
    }
  }


  void Kademlia::OnRequest(ContactPtr from, PackagePtr request)
  {
    THREAD_ENSURE(this->thread.get(), OnRequest, from, request);

    Instruction * instr = request->GetInstruction();

    switch (instr->Code())
    {
      case OpCode::PING:
      {
        this->OnRequestPing(from, request);
        break;
      }

      case OpCode::FIND_NODE:
      {
        this->OnRequestFindNode(from, request);
        break;
      }

      case OpCode::FIND_VALUE:
      {
        this->OnRequestFindValue(from, request);
        break;
      }

      case OpCode::STORE:
      {
        this->OnRequestStore(from, request);
        break;
      }

      default:
        printf("Unknown instruction received: code=%u\n", static_cast<unsigned>(instr->Code()));
        break;
    }
  }


  void Kademlia::OnRequestPing(ContactPtr from, PackagePtr request)
  {
    this->dispatcher->Send(std::make_shared<Package>(
      Package::PackageType::Response,
      Config::NodeId(),
      request->Id(),
      from,
      std::unique_ptr<Instruction>(new protocol::Pong())
    ));
  }


  void Kademlia::OnRequestFindNode(ContactPtr from, PackagePtr request)
  {
    protocol::FindNode * reqInstr = static_cast<protocol::FindNode *>(request->GetInstruction());

    auto target = reqInstr->Key();

    std::vector<std::pair<KeyPtr, ContactPtr>> result;

    this->kBuckets->FindClosestContacts(target, result);

    protocol::FindNodeResponse * resInstr = new protocol::FindNodeResponse();

    for (const auto & node : result)
    {
      resInstr->AddNode(node.first, node.second);
    }

    this->dispatcher->Send(std::make_shared<Package>(Package::PackageType::Response, Config::NodeId(), request->Id(), from, std::unique_ptr<Instruction>(resInstr)));
  }


  void Kademlia::OnRequestFindValue(ContactPtr from, PackagePtr request)
  {
    protocol::FindValue * reqInstr = static_cast<protocol::FindValue *>(request->GetInstruction());

    Storage storage;

    storage.SetKey(reqInstr->Key());

    if (storage.Load())
    {
      protocol::FindValueResponse * resInstr = new protocol::FindValueResponse();

      resInstr->SetData(storage.GetData());

      this->dispatcher->Send(std::make_shared<Package>(Package::PackageType::Response, Config::NodeId(), request->Id(), from, std::unique_ptr<Instruction>(resInstr)));
    }
    else
    {
      this->OnRequestFindNode(from, request);
    }
  }


  void Kademlia::OnRequestStore(ContactPtr from, PackagePtr request)
  {
    protocol::Store * reqInstr = static_cast<protocol::Store *>(request->GetInstruction());

    Storage storage;

    storage.SetKey(reqInstr->GetKey());

    storage.SetData(reqInstr->Data());

    bool result = storage.Save();

    protocol::StoreResponse * resInstr = new protocol::StoreResponse();

    resInstr->SetResult(result ? protocol::StoreResponse::ErrorCode::SUCCESS : protocol::StoreResponse::ErrorCode::FAILED);

    this->dispatcher->Send(std::make_shared<Package>(Package::PackageType::Response, Config::NodeId(), request->Id(), from, std::unique_ptr<Instruction>(resInstr)));
  }


  bool Kademlia::InitBuckets()
  {
    TSTRING bucketsFilePath = Config::RootPath() + _T(PATH_SEPERATOR_STR) + _T("contacts");

    // TODO: serialize contacts
    uint8_t buffer[Key::KEY_LEN + sizeof(Contact)];

    FILE * file = _tfopen(bucketsFilePath.c_str(), _T("r"));
    if (!file)
    {
      bucketsFilePath = Config::RootPath() + _T(PATH_SEPERATOR_STR) + _T("default_contacts");

      file = _tfopen(bucketsFilePath.c_str(), _T("r"));
      if (!file)
      {
        return false;
      }
    }

    while (fread(buffer, 1, sizeof(buffer), file) == sizeof(buffer))
    {
      auto key = std::make_shared<Key>(buffer);
      auto contact = std::make_shared<Contact>(* (reinterpret_cast<Contact *>(buffer + Key::KEY_LEN)));

      this->kBuckets->AddContact(key, contact);
    }

    fclose(file);

    if (this->kBuckets->Size() == 0)
    {
      printf("WARNING: no initial buckets found. This should only be used on the root node.\n");
    }

    return true;
  }


  void Kademlia::SaveBuckets()
  {
    TSTRING bucketsFilePath = Config::RootPath() + _T(PATH_SEPERATOR_STR) + _T("contacts");

    FILE * file = _tfopen(bucketsFilePath.c_str(), _T("r"));

    if (file)
    {
      std::vector<std::pair<KeyPtr, ContactPtr>> entries;

      this->kBuckets->GetAllContacts(entries);

      for (const auto & entry : entries)
      {
        fwrite(entry.first->Buffer(), 1, Key::KEY_LEN, file);
        fwrite(entry.second.get(), 1, sizeof(Contact), file);
      }      

      fclose(file);
    }
  }


  void Kademlia::PrintNodes() const
  {
    if (this->thread.get() != Thread::Current())
    {
      this->thread->Invoke([this](void *, void *) { this->PrintNodes(); });
      return;
    }

    std::vector<std::pair<KeyPtr, ContactPtr>> nodes;

    this->kBuckets->GetAllContacts(nodes);

    printf("{ \"nodes\":\n");

    printf("[\n");

    for (const auto & node : nodes)
    {
      printf(" { \"key\" : \"%s\", \"addr\" : \"%s\" }\n", node.first->ToString().c_str(), node.second->ToString().c_str());
    }

    printf("], \"count\":%llu }\n", (long long unsigned)nodes.size());
    
  }
}