////////////////////////////////////////////////
//   
//    File: TriggerDecoder_tool.cc
//       
//    Description: Starting point for extracting ICARUS trigger fragment information into LArSoft object TBD 
//
//    Author: Jacob Zettlemoyer, FNAL
//
///////////////////////////////////////////////

#include "art/Framework/Core/EDProducer.h"
#include "art/Framework/Principal/Event.h"
#include "art/Framework/Principal/Handle.h"
#include "art/Framework/Services/Registry/ServiceHandle.h"
#include "art/Persistency/Common/PtrMaker.h"
#include "art/Utilities/ToolMacros.h"
#include "cetlib/cpu_timer.h"
#include "fhiclcpp/ParameterSet.h"
#include "messagefacility/MessageLogger/MessageLogger.h"

#include "lardata/DetectorInfoServices/DetectorClocksService.h"
#include "lardataalg/DetectorInfo/DetectorTimings.h"
#include "lardataalg/Utilities/quantities/spacetime.h" // util::quantities::nanosecond
#include "lardataobj/RawData/ExternalTrigger.h" //JCZ: TBD, placeholder for now to represent the idea
#include "lardataobj/RawData/TriggerData.h" // raw::Trigger
#include "lardataobj/Simulation/BeamGateInfo.h" //JCZ:, another placeholder I am unsure if this and above will be combined at some point into a dedicated object 

#include "sbndaq-artdaq-core/Overlays/ICARUS/ICARUSTriggerUDPFragment.hh"

// #include "sbnobj/Common/Trigger/ExtraTriggerInfo.h" // maybe future location of:
#include "icaruscode/Decode/DataProducts/ExtraTriggerInfo.h"
#include "icaruscode/Decode/DecoderTools/IDecoder.h"
// #include "sbnobj/Common/Trigger/BeamBits.h" // maybe future location of:
#include "icaruscode/Decode/BeamBits.h" // sbn::triggerSource
#include "icaruscode/Decode/DecoderTools/Dumpers/FragmentDumper.h" // dumpFragment()
#include "icaruscode/Decode/DecoderTools/details/KeyedCSVparser.h"
#include "icarusalg/Utilities/BinaryDumpUtils.h" // hexdump() DEBUG

#include <cstdlib>
#include <iostream>
#include <string_view>
#include <memory>


using namespace std::string_literals;

namespace daq 
{
  
  // --- BEGIN -- TEMPORARY ----------------------------------------------------
  // remove when SBNSoftware/sbndaq-artdaq-core pull request #31 is in:
  // https://github.com/SBNSoftware/sbndaq-artdaq-core/pull/31
  std::uint64_t getNanoseconds_since_UTC_epoch_fix
    (icarus::ICARUSTriggerInfo const& info)
  {
    
    if(info.wr_seconds == -2 || info.wr_nanoseconds == -3)
      return 0;
    int correction = 0;
    if(info.wr_seconds >= 1483228800)
      correction = 37;
    uint64_t const corrected_ts
      { (info.wr_seconds-correction)*1'000'000'000ULL + info.wr_nanoseconds };
    return corrected_ts;

  } // getNanoseconds_since_UTC_epoch_fix()
  // --- END ---- TEMPORARY ----------------------------------------------------
  
  
  /**
   * @brief Tool decoding the trigger information from DAQ.
   * 
   * Produces:
   * * `std::vector<raw::ExternalTrigger>` containing the absolute trigger time
   *     stamp from the White Rabbit system, and a trigger count;
   *     it always includes a single entry (zero _might_ be supported).
   * * `std::vector<raw::Trigger>` containing:
   *     * `TriggerTime()`: the relative time of the trigger as reported in the
   *         White Rabbit timestamp, measured in the
   *         @ref DetectorClocksElectronicsTime "electronics time scale" (for
   *         ICARUS it will always be
   *         `detinfo::DetectorClocksData::TriggerTime()`).
   *     * `BeamGateTime()`: relative time of the announced arrival of the beam
   *         (currently not available) also in
   *         @ref DetectorClocksElectronicsTime "electronics time scale".
   *     * `TriggerCount()`: the trigger count from the beginning of the run.
   *     * `TriggerBits()`: includes the beam(s) with an open gate when the
   *         trigger happened (currently only one beam gate per trigger);
   *         definitions are in `sbn::beamType` namespace.
   * 
   *     It always includes a single entry (zero _might_ be supported).
   * * `std::vector<sim::BeamGateInfo>` containing information on the "main"
   *     beam gate associated to each trigger (a way to say that if by any
   *     chance there are more than one bits set for the trigger, this gate
   *     will pick only one of them):
   *     * `Start()`: relative time of the announced arrival of the beam
   *         (currently not available), in
   *         @ref DetectorClocksSimulationTime "simulation time scale".
   *     * `Width()`: duration of the gate, in nanoseconds; currently set to a
   *         nominal value.
   *     * `BeamType()`: the type of the beam gate being described (BNB, NuMI).
   * * `sbn::ExtraTriggerInfo`: the most complete collection of information,
   *     duplicating also some from the other data products. Some of the
   *     information is not available yet: if a count is not available, its
   *     value is set to `0` (which is an invalid value because their valid
   *     range starts with `1` since they include the current event), and if a
   *     timestamp is not available it is set to
   *     `sbn::ExtraTriggerInfo::NoTimestamp`; these two conditions can be
   *     checked with static methods 
   *     `sbn::ExtraTriggerInfo::isValidTimestamp()` and 
   *     `sbn::ExtraTriggerInfo::isValidCount()` respectively.
   *     Note that differently from the usual, this is a _single object_, not
   *     a collection; also, this data product has no instance name.
   *     The information already available includes:
   *     * `sourceType`: the type of beam or trigger source, a value from
   *         `sbn::triggerSource` (equivalent to `raw::Trigger::TriggerBits()`,
   *         but in the form of an enumerator rather than a bit mask).
   *     * `triggerTimestamp`: same as `raw::ExternalTrigger::GetTrigTime()`
   *         (nanoseconds from the Epoch, Coordinated Universal Time).
   *     * `beamGateTimestamp`: absolute time of the beam gate opening as
   *         reported by the trigger hardware, directly comparable with
   *         `triggerTimestamp` (same scale and unit).
   *     * `triggerID`: same as `raw::ExternalTrigger::GetTrigID()`. Should
   *         match the event number.
   *     * `gateID`: the count of gates since the beginning of the run, as
   *         reported by the trigger hardware.
   * 
   * Besides the main data product (empty instance name) an additional
   * `std::vector<raw::ExternalTrigger>` data product with instance name
   * `"previous"` is also produced, which relays the same kind of information
   * but for the _previous_ trigger. This information also comes from the
   * trigger DAQ. If no previous trigger is available, this collection will be
   * empty.
   * 
   * 
   */
  class TriggerDecoder : public IDecoder
  {
    using nanoseconds = util::quantities::nanosecond;
  public:
    explicit TriggerDecoder(fhicl::ParameterSet const &pset);
    
    virtual void produces(art::ProducesCollector&) override;
    virtual void configure(const fhicl::ParameterSet&) override;
    virtual void initializeDataProducts() override;
    virtual void process_fragment(const artdaq::Fragment &fragment) override;
    virtual void outputDataProducts(art::Event &event) override;
   
  private: 
    using TriggerCollection = std::vector<raw::ExternalTrigger>;
    using TriggerPtr = std::unique_ptr<TriggerCollection>;
    using RelativeTriggerCollection = std::vector<raw::Trigger>;
    using BeamGateInfoCollection = std::vector<sim::BeamGateInfo>;
    using BeamGateInfoPtr = std::unique_ptr<BeamGateInfoCollection>;
    using ExtraInfoPtr = std::unique_ptr<sbn::ExtraTriggerInfo>;
    TriggerPtr fTrigger;
    TriggerPtr fPrevTrigger;
    std::unique_ptr<RelativeTriggerCollection> fRelTrigger;
    ExtraInfoPtr fTriggerExtra;
    BeamGateInfoPtr fBeamGateInfo; 
    bool fDiagnosticOutput; //< Produces large number of diagnostic messages, use with caution!
    bool fDebug; //< Use this for debugging this tool
    int fOffset; //< Use this to determine additional correction needed for TAI->UTC conversion from White Rabbit timestamps. Needs to be changed if White Rabbit firmware is changed and the number of leap seconds changes! 
    //Add in trigger data member information once it is selected, current LArSoft object likely not enough as is
    
    // uint64_t fLastTimeStamp = 0;
   
    long fLastEvent = 0;
    
    detinfo::DetectorTimings const fDetTimings; ///< Detector clocks and timings.
    
    /// Creates a `ICARUSTriggerInfo` from a generic fragment.
    icarus::ICARUSTriggerUDPFragment makeTriggerFragment
      (artdaq::Fragment const& fragment) const;
    
    /// Parses the trigger data packet with the "standard" parser.
    icarus::ICARUSTriggerInfo parseTriggerString(std::string_view data) const;
    
    /// Name of the data product instance for the current trigger.
    static std::string const CurrentTriggerInstanceName;
    
    /// Name of the data product instance for the previous trigger.
    static std::string const PreviousTriggerInstanceName;
    
    static constexpr double UnknownBeamTime = std::numeric_limits<double>::max();
    
    /// Codes of gate types from the trigger hardware.
    struct TriggerGateTypes {
      static constexpr int BNB { 1 };
      static constexpr int NuMI { 2 };
    }; 
    
    static constexpr nanoseconds BNBgateDuration { 1600. };
    static constexpr nanoseconds NuMIgateDuration { 9500. };
    
    static std::string_view firstLine
      (std::string const& s, std::string const& endl = "\0\n\r"s);
    
    /// Combines second and nanosecond counts into a 64-bit timestamp.
    static std::uint64_t makeTimestamp(unsigned int s, unsigned int ns)
      { return s * 1'000'000'000ULL + ns; }
    /// Returns the difference `a - b`.
    static long long int timestampDiff(std::uint64_t a, std::uint64_t b)
      { return static_cast<long long int>(a) - static_cast<long long int>(b); }
    
  };


  std::string const TriggerDecoder::CurrentTriggerInstanceName {};
  std::string const TriggerDecoder::PreviousTriggerInstanceName { "previous" };
  

  TriggerDecoder::TriggerDecoder(fhicl::ParameterSet const &pset)
    : fDetTimings
      { art::ServiceHandle<detinfo::DetectorClocksService>()->DataForJob() }
  {
    this->configure(pset);
  }

  
  void TriggerDecoder::produces(art::ProducesCollector& collector) 
  {
    collector.produces<TriggerCollection>(CurrentTriggerInstanceName);
    collector.produces<TriggerCollection>(PreviousTriggerInstanceName);
    collector.produces<RelativeTriggerCollection>(CurrentTriggerInstanceName);
    collector.produces<BeamGateInfoCollection>(CurrentTriggerInstanceName);
    collector.produces<sbn::ExtraTriggerInfo>(CurrentTriggerInstanceName);
  }
    

  void TriggerDecoder::configure(fhicl::ParameterSet const &pset) 
  {
    fDiagnosticOutput = pset.get<bool>("DiagnosticOutput", false);
    fDebug = pset.get<bool>("Debug", false);
    fOffset = pset.get<long long int>("TAIOffset", 2'000'000'000);
    return;
  }
  
  void TriggerDecoder::initializeDataProducts()
  {
    //use until different object chosen 
    //fTrigger = new raw::Trigger();
    fTrigger = std::make_unique<TriggerCollection>();
    fPrevTrigger = std::make_unique<TriggerCollection>();
    fRelTrigger = std::make_unique<RelativeTriggerCollection>();
    fBeamGateInfo = BeamGateInfoPtr(new BeamGateInfoCollection);
    fTriggerExtra = std::make_unique<sbn::ExtraTriggerInfo>();
    return;
  }
  
  
  icarus::ICARUSTriggerUDPFragment TriggerDecoder::makeTriggerFragment
    (artdaq::Fragment const& fragment) const
  {
    try {
      return icarus::ICARUSTriggerUDPFragment { fragment };
    }
    catch(std::exception const& e) {
      mf::LogSystem("TriggerDecoder")
        << "Error while creating trigger fragment from:\n"
          << sbndaq::dumpFragment(fragment)
        << "\nError message: " << e.what();
      throw;
    }
    catch(...) {
      mf::LogSystem("TriggerDecoder")
        << "Unidentified exception while creating trigger fragment from:"
          << sbndaq::dumpFragment(fragment);
      throw;
    }
  } // TriggerDecoder::parseTriggerString()

  
  icarus::ICARUSTriggerInfo TriggerDecoder::parseTriggerString
    (std::string_view data) const
  {
    try {
      return icarus::parse_ICARUSTriggerString(data.data());
    }
    catch(std::exception const& e) {
      mf::LogSystem("TriggerDecoder")
        << "Error while running standard parser on " << data.length()
        << "-char long trigger string:\n==>|" << data
        << "|<==\nError message: " << e.what();
      throw;
    }
    catch(...) {
      mf::LogSystem("TriggerDecoder")
        << "Unidentified exception while running standard parser on "
        << data.length() << "-char long trigger string:\n==>|" << data << "|.";
      throw;
    }
  } // TriggerDecoder::parseTriggerString()

  

  void TriggerDecoder::process_fragment(const artdaq::Fragment &fragment)
  {
    uint64_t artdaq_ts = fragment.timestamp();
    icarus::ICARUSTriggerUDPFragment frag { makeTriggerFragment(fragment) };
    std::string data = frag.GetDataString();
    char *buffer = const_cast<char*>(data.c_str());
    // --- BEGIN -- TEMPORARY --------------------------------------------------
    // restore OLD CODE when SBNSoftware/sbndaq-artdaq-core pull request #31 is in:
    // https://github.com/SBNSoftware/sbndaq-artdaq-core/pull/31
    icarus::ICARUSTriggerInfo datastream_info = parseTriggerString(buffer);
    uint64_t wr_ts = getNanoseconds_since_UTC_epoch_fix(datastream_info) + fOffset;
    // OLD CODE:
//     uint64_t wr_ts = datastream_info.getNanoseconds_since_UTC_epoch() + fOffset;
    // --- END ---- TEMPORARY --------------------------------------------------
    int gate_type = datastream_info.gate_type;
    long delta_gates_bnb [[maybe_unused]] = frag.getDeltaGatesBNB();
    long delta_gates_numi [[maybe_unused]] = frag.getDeltaGatesOther(); //this is actually NuMI due to abrupt changes in trigger board logic
    long delta_gates_other [[maybe_unused]] = frag.getDeltaGatesNuMI();
    uint64_t lastTrigger = 0;
    
    // --- BEGIN -- TEMPORARY --------------------------------------------------
    // remove this part when the beam gate timestamp is available via fragment
    // or via the parser; the beam gate (`beamgate_ts`) is supposed to undergo
    // the same correction as the trigger time (`wr_ts`)
    auto const parsedData = icarus::details::KeyedCSVparser{}(firstLine(data));
    unsigned int beamgate_count { std::numeric_limits<unsigned int>::max() };
    std::uint64_t beamgate_ts { wr_ts }; // we cheat
    /* [20210717, petrillo@slac.stanford.edu] `(pBeamGateInfo->nValues() == 3)`:
     * this is an attempt to "support" a few Run0 runs (6017 to roughly 6043)
     * which have the beam gate information truncated; this workaround should
     * be removed when there is enough ICARUS data that these runs become
     * uninteresting.
     */
    if (auto pBeamGateInfo = parsedData.findItem("Beam_TS");
      pBeamGateInfo && (pBeamGateInfo->nValues() == 3)
    ) {
      // if gate information is found, it must be complete
      beamgate_count = pBeamGateInfo->getNumber<unsigned int>(0U);
      // use the same linear correction for the beam gate timestamp as it was
      // used for the trigger one; here we assume that wr_ts
      // (current value of beamgate_ts) is corrected, while
      // `getWRSeconds()` and `getWRNanoSeconds()` are as corrected as `Beam_TS`
      // (i.e. both not corrected)
      beamgate_ts += makeTimestamp(
        pBeamGateInfo->getNumber<unsigned int>(1U),
        pBeamGateInfo->getNumber<unsigned int>(2U)
        )
        - makeTimestamp(frag.getWRSeconds(), frag.getWRNanoSeconds())
        ;
    } // if has gate information
    // --- END ---- TEMPORARY --------------------------------------------------
    
    if(fDiagnosticOutput || fDebug)
    {
      std::cout << "Full Timestamp = " << wr_ts << std::endl;
      auto const cross_check = static_cast<signed long long int>(wr_ts)
        - static_cast<signed long long int>(artdaq_ts);
      if(abs(cross_check) > 1000)
        std::cout << "Loaded artdaq TS and fragment data TS are > 1 ms different! They are " << cross_check << " nanoseconds different!" << std::endl;
      std::cout << "Beam gate " << beamgate_count << " at "
        << (beamgate_ts/1'000'000'000) << "." << (beamgate_ts%1'000'000'000)
        << " s (" << timestampDiff(beamgate_ts, wr_ts)
        << " ns relative to trigger)" << std::endl;
      
      // note that this parsing is independent from the one used above
      std::string_view const dataLine = firstLine(data);
      try {
        auto const parsedData = icarus::details::KeyedCSVparser{}(dataLine);
        std::cout << "Parsed data (from " << dataLine.size() << " characters): "
          << parsedData << std::endl;
      }
      catch(icarus::details::KeyedCSVparser::Error const& e) {
        mf::LogError("TriggerDecoder")
          << "Error parsing " << dataLine.length()
          << "-char long trigger string:\n==>|" << dataLine
          << "|<==\nError message: " << e.what() << std::endl;
        throw;
      }
      
      if (fDebug) { // this grows tiresome quickly when processing many events
        std::cout << "Trigger packet content:\n" << dataLine
          << "\nFull trigger fragment dump:"
          << sbndaq::dumpFragment(fragment) << std::endl;
      }
    }
    
    //
    // extra trigger info
    //
    sbn::triggerSource beamGateBit;
    switch (gate_type) {
      case TriggerGateTypes::BNB:  beamGateBit = sbn::triggerSource::BNB;  break;
      case TriggerGateTypes::NuMI: beamGateBit = sbn::triggerSource::NuMI; break;
      default:                     beamGateBit = sbn::triggerSource::Unknown;
    } // switch gate_type
    
    fTriggerExtra->sourceType = beamGateBit;
    fTriggerExtra->triggerTimestamp = wr_ts;
    fTriggerExtra->beamGateTimestamp = beamgate_ts;
    fTriggerExtra->triggerID = datastream_info.wr_event_no;
    fTriggerExtra->gateID = datastream_info.gate_id;
    /* TODO:
    fTriggerExtra->triggerCount
    fTriggerExtra->gateCount
    fTriggerExtra->gateCountFromPreviousTrigger
    fTriggerExtra->anyTriggerCountFromPreviousTrigger
    fTriggerExtra->anyGateCountFromPreviousTrigger
    fTriggerExtra->anyPreviousTriggerSourceType
    fTriggerExtra->anyGateCountFromAnyPreviousTrigger
    fTriggerExtra->previousTriggerTimestamp
    fTriggerExtra->anyPreviousTriggerTimestamp
    */
    
    //
    // absolute time trigger (raw::ExternalTrigger)
    //
    fTrigger->emplace_back
      (fTriggerExtra->triggerID, fTriggerExtra->triggerTimestamp);
    
    //
    // previous absolute time trigger (raw::ExternalTrigger)
    //
    if(fTriggerExtra->triggerID == 1)
    {
      fLastEvent = 0;
    }
    else 
    {
      fLastEvent = fTriggerExtra->triggerID - 1;
      lastTrigger = frag.getLastTimestampBNB();
      fPrevTrigger->emplace_back(fLastEvent, lastTrigger);
    }
    
    //
    // beam gate
    //
    // beam gate - trigger: hope it's negative...
    nanoseconds const gateStartFromTrigger{
      static_cast<double>(timestampDiff
        (fTriggerExtra->beamGateTimestamp, fTriggerExtra->triggerTimestamp)
        )
      }; // narrowing!!
    auto const elecGateStart = fDetTimings.TriggerTime() + gateStartFromTrigger;
    auto const simGateStart = fDetTimings.toSimulationTime(elecGateStart);
    switch (gate_type) {
      case TriggerGateTypes::BNB:
        fBeamGateInfo->emplace_back
          (simGateStart.value(), BNBgateDuration.value(), sim::kBNB);
        break;
      case TriggerGateTypes::NuMI:
        fBeamGateInfo->emplace_back
          (simGateStart.value(), NuMIgateDuration.value(), sim::kNuMI);
        break;
      default:
        mf::LogWarning("TriggerDecoder") << "Unsupported gate type #" << gate_type;
    } // switch gate_type
    
    //
    // relative time trigger (raw::Trigger)
    //
    fRelTrigger->emplace_back(
      static_cast<unsigned int>(datastream_info.wr_event_no), // counter
      fDetTimings.TriggerTime().value(),                      // trigger_time
      elecGateStart.value(),                                  // beamgate_time
      mask(beamGateBit)                                       // bits
      );
    
    //Once we have full trigger data object, set up and place information into there
    return;
  }

  void TriggerDecoder::outputDataProducts(art::Event &event)
  {
    //Place trigger data object into raw data store 
    event.put(std::move(fTrigger), CurrentTriggerInstanceName);
    event.put(std::move(fRelTrigger), CurrentTriggerInstanceName);
    event.put(std::move(fPrevTrigger), PreviousTriggerInstanceName);
    event.put(std::move(fBeamGateInfo), CurrentTriggerInstanceName);
    event.put(std::move(fTriggerExtra));
    return;
  }

  std::string_view TriggerDecoder::firstLine
    (std::string const& s, std::string const& endl /* = "\0\n\r" */)
  {
    return { s.data(), std::min(s.find_first_of(endl), s.size()) };
  }
  
  
  DEFINE_ART_CLASS_TOOL(TriggerDecoder)

}





  
