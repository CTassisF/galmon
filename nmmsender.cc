#include "nmmsender.hh"
#include "comboaddress.hh"
#include "swrappers.hh"
#include "sclasses.hh"
#include <random>
#include "navmon.hh"
#include <algorithm>
#include "zstdwrap.hh"

using namespace std;

void NMMSender::sendTCPThread(Destination* d)
{
  struct NameError{};
  for(;;) {
    ComboAddress chosen;
    map<uint32_t, string> unacked;
    try {
      vector<ComboAddress> addrs;
      for(;;) {
        addrs=resolveName(d->dst, true, true);
        if(!addrs.empty())
          break;
          
        cerr<<humanTimeNow()<<" Unable to resolve "<<d->dst<<", sleeping and trying again later"<<endl;
        throw NameError();
      }
          
      std::random_device rng;
      std::mt19937 urng(rng());
      std::shuffle(addrs.begin(), addrs.end(), urng);

      for(auto& addr: addrs)  {
        if(!addr.sin4.sin_port)
          addr.sin4.sin_port = ntohs(29603);
        chosen=addr;
        Socket s(addr.sin4.sin_family, SOCK_STREAM);
        SocketCommunicator sc(s);
        sc.setTimeout(3);
        sc.connect(addr);
        time_t connStartTime = time(0);
        if (d_debug) { cerr<<humanTimeNow()<<" Connected to "<<d->dst<<" on "<<addr.toStringWithPort()<<endl; }
        auto emit = [&sc](const char*buf, uint32_t len) {
          sc.writen(string(buf, len));
        };
        std::unique_ptr<ZStdCompressor> zsc;
        if(d_compress) {
          sc.writen("RNIE"); // the other magic value is "bert". hence. 
          zsc = std::make_unique<ZStdCompressor>(emit, 20);
        }
        bool hadMessage=false;
        int msgnum = 0;

        for(;;) {
          uint32_t num;
          // read acks
          for(;;) {
            int res = read(s, &num, 4);
            if(res < 0) {
              if(errno != EAGAIN)
                unixDie("Reading acknowledgements in nmmsender");
              break;
            }
            if(res==0)
              throw std::runtime_error("EOF while reading acks");
            if(res==4) {
              num = ntohl(num);
              unacked.erase(num);
            }
            else
              throw std::runtime_error("Partial read of "+to_string(res)+" bytes");
          }

          
          std::string msg;
          {
            std::lock_guard<std::mutex> mut(d->mut);
            if(!d->queue.empty()) {
              msg = d->queue.front();
            }
          }
          if(!msg.empty()) {
            hadMessage=true;
            if(zsc) {
              
              uint32_t num = htonl(msgnum);
              string encap((const char*)&num, 4);
              encap += msg;
              zsc->give(encap.c_str(), encap.size());
              unacked[msgnum] = msg;
              msgnum++;

            }
            else
              sc.writen(msg);
            std::lock_guard<std::mutex> mut(d->mut);
            d->queue.pop_front();

          }
          else {
            if(zsc && hadMessage) {
              cerr << "Compressed to: "<< 100.0*zsc->d_outputBytes/zsc->d_inputBytes<<"%, buffered compressed: "<<zsc->outputBufferBytes()<<" out of " <<zsc->outputBufferCapacity()<<" bytes. Unacked: "<<unacked.size()<<endl;

              zsc->flush();

              if(time(0) - connStartTime > 10 && unacked.size() > 1000)
                throw std::runtime_error("Too many messages unacked, recycling connection");


              
            }
            hadMessage = false;
            usleep(100000);

          }
        }
      }
    }
    catch(NameError&) {
      {
        std::lock_guard<std::mutex> mut(d->mut);
        if (d_debug) { cerr<<humanTimeNow()<<" There are now "<<d->queue.size()<<" messages queued for "<<d->dst<<", and "<<unacked.size()<<" unacknowledged"<<endl; }
      }
      sleep(30);
    }
    catch(std::exception& e) {
      if (d_debug) { cerr<<humanTimeNow()<<" Sending thread for "<<d->dst<<" via "<<chosen.toStringWithPort()<<" had error: "<<e.what()<<endl; }
      {
        std::lock_guard<std::mutex> mut(d->mut);
        if (d_debug) { cerr<<humanTimeNow()<<" There are now "<<d->queue.size()<<" messages queued for "<<d->dst<<", and "<<unacked.size()<<" unacknowledged"<<endl; }
      }
      sleep(1);
    }
    catch(...) {
      if (d_debug) { cerr<<humanTimeNow()<<" Sending thread for "<<d->dst <<" via "<<chosen.toStringWithPort()<<" had error"; }
      {
        std::lock_guard<std::mutex> mut(d->mut);
        if (d_debug) { cerr<<"There are now "<<d->queue.size()<<" messages queued for "<<d->dst<<", and "<<unacked.size()<<" unacknowledge via "<<chosen.toStringWithPort()<<endl; }
      }
      sleep(1);
    }
    std::lock_guard<std::mutex> mut(d->mut);
    if(!unacked.empty()) {
      cerr<<humanTimeNow()<< " Stuffing "<<unacked.size()<<" messages back into the queue"<<endl;
      for(auto iter= unacked.rbegin(); iter != unacked.rend(); ++iter) {
        d->queue.push_front(iter->second);
      }
      unacked.clear();
    }
  }
}


void NMMSender::emitNMM(const NavMonMessage& nmm)
{
  for(auto& d : d_dests) {
    d->emitNMM(nmm, d_compress);
  }
}

void NMMSender::Destination::emitNMM(const NavMonMessage& nmm, bool compressed)
{
  string out;
  nmm.SerializeToString(& out);
  string msg;
  if(!compressed)
    msg="bert";
  
  uint16_t len = htons(out.size());
  msg.append((char*)&len, 2);
  msg.append(out);

  if(!dst.empty()) {
    std::lock_guard<std::mutex> l(mut);
    queue.push_back(msg);
  }
  else
    writen2(fd, msg.c_str(), msg.size());
}

