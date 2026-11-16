/* SPDX-License-Identifier: BSD-2-Clause-Patent */
#pragma once
#include "DitingPanelPlans.h"

namespace DitingPanel {
struct Diagnostic {
    uint32_t version=3,bytes=sizeof(Diagnostic),sequence=0,stage=0;
    uint32_t error=0,panel=0,targetHz=0,rollbackHz=0;
    uint32_t id[3]{255,255,255},before=255,after=255,batches=0,restoring=0,restored=0;
    uint64_t startUs=0,endUs=0;
    uint32_t returnSizeCommands=0,queryRegister=0,readControl=0,data0=0,data1=0;
    uint32_t addressMode=255,pixelFormat=255,colourError=0;
    uint32_t modelHigh=0,modelLow=0,selectionReason=0;
    uint32_t addressAfter=255,colourWrites=0,colourRestored=0,colourStatus=0;
    uint32_t responses[9][4]{}; // register, read control, DATA0, DATA1 before retirement
};
static_assert(sizeof(Diagnostic)==288,"Diting panel mode diagnostic ABI");
struct Apply {void *context;bool (*tear)(void *,unsigned);};

// Runs on the backlight work loop and borrows its Command between completed
// brightness operations. An unretired timeout retains the same FIFO owner.
class Mode {
    enum Phase {Idle,Da,Db,Dc,Address,Pixel,ColourWrite,ColourVerify,
                ColourRestoreWrite,ColourRestoreRead,Before,Write,After,Tear,Finished,Retained};
    Phase phase_=Idle,retainedPhase_=Idle;
    DitingBrightness::Command *command_=nullptr;
    DitingBrightness::Io io_{};
    Apply apply_{};
    Selection selection_{};
    Diagnostic d_{};
    Plan plan_{};
    unsigned cursor_=0,addressTarget_=255;
    bool touched_=false,recovering_=false,colourTouched_=false,colourRecovering_=false;
    bool read_=false,sizeReady_=false,negotiating_=false;
    uint64_t deadline_=0,wait_=0;
    bool colourPhase(Phase phase) const {
        return phase==ColourWrite || phase==ColourVerify ||
               phase==ColourRestoreWrite || phase==ColourRestoreRead;
    }
    void next(Phase phase,uint64_t now) {
        phase_=phase;wait_=0;deadline_=now+200000000ULL;
    }
    bool finish(uint64_t now) {
        phase_=Finished;d_.stage=8;d_.endUs=now/1000;return false;
    }
    bool colourFailure(unsigned flag,uint64_t now) {
        d_.colourError|=flag;negotiating_=false;sizeReady_=false;
        if(colourTouched_ && !colourRecovering_) {
            colourRecovering_=true;next(ColourRestoreWrite,now);return true;
        }
        d_.colourStatus=4;
        if(!d_.error)d_.error=14;
        return finish(now);
    }
    bool failure(unsigned error,uint64_t now) {
        if(colourPhase(phase_))return colourFailure(error==4?8u:4u,now);
        negotiating_=false;sizeReady_=false;
        if(!d_.error)d_.error=error;
        if(touched_ && d_.rollbackHz && !recovering_) {
            recovering_=true;d_.restoring=1;d_.stage=7;
            if(plan(static_cast<Panel>(d_.panel),d_.rollbackHz,plan_)) {
                cursor_=0;next(Write,now);return true;
            }
        }
        return finish(now);
    }
    unsigned target() const {return recovering_?d_.rollbackHz:d_.targetHz;}
    void afterPixel(uint64_t now) {
        if(d_.error || !d_.panel) {finish(now);return;}
        // DCS SET_ADDRESS_MODE (36) changes BGR in bit3. The verified DT
        // requires RGB. Preserve every orientation/scan bit from GET(0b).
        // No colour write is allowed with an unsupported/malformed query,
        // reserved address bits, or an unqualified pixel-mode response.
        if(!d_.colourError && d_.addressMode!=255 && !(d_.addressMode&3u) && d_.pixelFormat==0x77) {
            d_.addressAfter=d_.addressMode;
            if(d_.addressMode&8u) {
                addressTarget_=d_.addressMode&~8u;next(ColourWrite,now);return;
            }
            d_.colourStatus=1;
        }
        next(Before,now);
    }
    unsigned replySlot() const {
        const Phase phase=phase_==Retained?retainedPhase_:phase_;
        if(phase==Da)return 0;
        if(phase==Db)return 1;
        if(phase==Dc)return 2;
        if(phase==Address)return 3;
        if(phase==Pixel)return 4;
        if(phase==Before)return 5;
        if(phase==After)return 6;
        return phase==ColourVerify?7:8;
    }
public:
    void initialize(DitingBrightness::Command &command,DitingBrightness::Io io,Apply apply,
                    Selection selection={}) {
        command_=&command;io_=io;apply_=apply;selection_=selection;
        d_.modelHigh=selection.modelHigh;d_.modelLow=selection.modelLow;
    }
    bool started() const {return phase_!=Idle;}
    bool active() const {return phase_!=Idle && phase_!=Finished;}
    bool retained() const {return phase_==Retained;}
    const Diagnostic &diagnostic() const {return d_;}
    bool start(unsigned hz,uint64_t now) {
        if(started() || !command_ || command_->inFlight() || !io_.read || !apply_.tear ||
           (hz!=60 && hz!=90 && hz!=120))return false;
        d_.targetHz=hz;d_.stage=1;d_.startUs=now/1000;next(Da,now);return true;
    }
    // Suggested next timer interval, or zero for completion/retained owner.
    unsigned step(uint64_t now) {
        if(!active())return 0;
        if(command_->inFlight()) {
            const auto completion=command_->poll(now);
            if(completion==DitingBrightness::Completion::Pending)return 1;
            if(completion==DitingBrightness::Completion::Retained) {
                if(phase_!=Retained)retainedPhase_=phase_;
                phase_=Retained;d_.stage=9;if(!d_.error)d_.error=10;
                if(colourPhase(retainedPhase_))d_.colourError|=32;
                d_.endUs=now/1000;return 0;
            }
            uint8_t value=0;bool valid=true;
            if(read_) {
                d_.readControl=io_.read(io_.context,DitingBrightness::ReadControl);
                d_.data0=io_.read(io_.context,DitingBrightness::ReadData0);
                d_.data1=io_.read(io_.context,DitingBrightness::ReadData1);
                auto &reply=d_.responses[replySlot()];
                reply[0]=d_.queryRegister;reply[1]=d_.readControl;reply[2]=d_.data0;reply[3]=d_.data1;
                valid=readByte(d_.readControl>>16,d_.data0,d_.data1,value);
            }
            if(!command_->finish()) {
                if(phase_!=Retained)retainedPhase_=phase_;
                phase_=Retained;d_.stage=9;d_.error=11;return 0;
            }
            if(phase_==Retained) {
                phase_=retainedPhase_;failure(10,now);return active()?1:0;
            }
            if(completion==DitingBrightness::Completion::Error || !valid) {
                if(read_ && (phase_==Da || phase_==Db || phase_==Dc)) {
                    // A retired failed ID query forbids timing writes, but
                    // still permits independent address/pixel observations.
                    if(!d_.error)d_.error=valid?3:4;
                    sizeReady_=false;
                    if(phase_==Dc) {d_.selectionReason=2;next(Address,now);}
                    else next(phase_==Da?Db:Dc,now);
                    return 1;
                }
                if(read_ && (phase_==Address || phase_==Pixel)) {
                    d_.colourError|=phase_==Address?1u:2u;sizeReady_=false;
                    if(phase_==Address)next(Pixel,now);else afterPixel(now);
                    return active()?1:0;
                }
                failure(valid?3:4,now);return active()?1:0;
            }
            if(negotiating_) {
                negotiating_=false;sizeReady_=true;++d_.returnSizeCommands;
            } else {
                if(read_)sizeReady_=false;
                switch(phase_) {
                case Da:d_.id[0]=value;next(Db,now);break;
                case Db:d_.id[1]=value;next(Dc,now);break;
                case Dc:
                    d_.id[2]=value;d_.panel=unsigned(identify(selection_,d_.id[0],d_.id[1],d_.id[2]));
                    d_.selectionReason=model(selection_)==Panel::Unknown?1:!d_.panel?2:0;
                    if(!d_.panel && !d_.error)d_.error=5;
                    d_.stage=2;next(Address,now);break;
                case Address:d_.addressMode=value;next(Pixel,now);break;
                case Pixel:d_.pixelFormat=value;afterPixel(now);break;
                case ColourWrite:
                    ++d_.colourWrites;next(ColourVerify,now);wait_=now+20000000ULL;break;
                case ColourVerify:
                    d_.addressAfter=value;
                    if(value!=addressTarget_)colourFailure(8,now);
                    else {d_.colourStatus=2;next(Before,now);}
                    break;
                case ColourRestoreWrite:
                    ++d_.colourWrites;next(ColourRestoreRead,now);wait_=now+20000000ULL;break;
                case ColourRestoreRead:
                    d_.addressAfter=value;
                    if(value!=d_.addressMode)colourFailure(16,now);
                    else {
                        d_.colourRestored=1;d_.colourStatus=3;
                        if(d_.error)finish(now);else next(Before,now);
                    }
                    break;
                case Before:
                    d_.before=value;d_.rollbackHz=modeRate(static_cast<Panel>(d_.panel),value);
                    if(!d_.rollbackHz) {failure(6,now);return 0;}
                    if(!plan(static_cast<Panel>(d_.panel),d_.targetHz,plan_)) {failure(7,now);return 0;}
                    if(value==plan_.expected) {d_.after=value;next(Tear,now);}
                    else {d_.stage=3;next(Write,now);}
                    break;
                case Write:
                    ++d_.batches;wait_=now+uint64_t(plan_.batch[cursor_].waitMs)*1000000ULL;
                    ++cursor_;deadline_=wait_+200000000ULL;break;
                case After:
                    d_.after=value;
                    if(value!=plan_.expected) {failure(8,now);return active()?1:0;}
                    next(Tear,now);break;
                default:failure(12,now);return active()?1:0;
                }
            }
        }
        if(!active() || phase_==Retained)return 0;
        if(now<wait_)return 1;
        if(phase_==Write && cursor_>=plan_.count) {d_.stage=recovering_?7:4;next(After,now);}
        if(phase_==Tear) {
            d_.stage=recovering_?7:5;
            if(!apply_.tear(apply_.context,target())) {failure(9,now);return active()?1:0;}
            phase_=Finished;d_.restored=recovering_;d_.stage=recovering_?8:6;d_.endUs=now/1000;return 0;
        }
        if(!command_->available()) {
            if(now<deadline_)return 1;
            failure(2,now);return active()?1:0;
        }
        bool submitted=false;
        if(phase_==Write) {
            const auto &b=plan_.batch[cursor_];
            submitted=command_->begin(b.words,b.count,false,false,now);read_=false;
            if(submitted)touched_=true;
        } else if(phase_==ColourWrite || phase_==ColourRestoreWrite) {
            const unsigned value=phase_==ColourWrite?addressTarget_:d_.addressMode;
            const uint32_t packet=0x80150036u|(value<<8);
            submitted=command_->begin(&packet,1,false,false,now);read_=false;
            if(submitted)colourTouched_=true;
        } else if(!sizeReady_) {
            // Retire a fresh one-byte limit before each BTA query. Brightness
            // has independently negotiated two bytes for GET(52).
            const uint32_t packet=0x80370001u;
            submitted=command_->begin(&packet,1,true,false,now);
            read_=false;negotiating_=submitted;
        } else {
            const unsigned reg=phase_==Da?0xda:phase_==Db?0xdb:phase_==Dc?0xdc:
                phase_==Address || phase_==ColourVerify || phase_==ColourRestoreRead?0x0b:
                phase_==Pixel?0x0c:0x2f;
            const uint32_t packet=0xa0060000u|reg;d_.queryRegister=reg;
            submitted=command_->begin(&packet,1,true,true,now);read_=true;
        }
        if(!submitted) {failure(2,now);return active()?1:0;}
        return 1;
    }
};
}
