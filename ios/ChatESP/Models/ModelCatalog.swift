import Foundation

enum ModelPurpose: String, CaseIterable, Identifiable {
    case chat
    case transcription
    case speech

    var id: String { rawValue }

    var title: String {
        switch self {
        case .chat: return "Chat and tools"
        case .transcription: return "Speech to text"
        case .speech: return "Text to speech"
        }
    }

    var explanation: String {
        switch self {
        case .chat:
            return "Understands the request, selects a tool when needed, and writes the answer. Requires text input, text output, and tool calling."
        case .transcription:
            return "Converts microphone audio into the text request. Requires audio input and transcription output."
        case .speech:
            return "Converts the final answer into spoken audio. Requires text input, speech output, and a published voice list."
        }
    }
}

enum SpeechVoiceLanguage: String, CaseIterable, Identifiable {
    case english
    case french

    var id: String { rawValue }

    var title: String {
        switch self {
        case .english: return "English voice"
        case .french: return "French voice"
        }
    }
}

struct OpenRouterModel: Decodable, Equatable, Identifiable {
    struct Architecture: Decodable, Equatable {
        let inputModalities: [String]
        let outputModalities: [String]

        private enum CodingKeys: String, CodingKey {
            case inputModalities = "input_modalities"
            case outputModalities = "output_modalities"
        }

        init(from decoder: Decoder) throws {
            let values = try decoder.container(keyedBy: CodingKeys.self)
            inputModalities = try values.decodeIfPresent(
                [String].self,
                forKey: .inputModalities
            ) ?? []
            outputModalities = try values.decodeIfPresent(
                [String].self,
                forKey: .outputModalities
            ) ?? []
        }
    }

    struct Pricing: Decodable, Equatable {
        let prompt: String?
        let completion: String?
        let request: String?
        let image: String?
        let webSearch: String?

        private enum CodingKeys: String, CodingKey {
            case prompt
            case completion
            case request
            case image
            case webSearch = "web_search"
        }
    }

    let id: String
    let name: String
    let description: String
    let architecture: Architecture
    let pricing: Pricing?
    let supportedParameters: [String]
    let supportedVoices: [String]

    private enum CodingKeys: String, CodingKey {
        case id
        case name
        case description
        case architecture
        case pricing
        case supportedParameters = "supported_parameters"
        case supportedVoices = "supported_voices"
    }

    init(from decoder: Decoder) throws {
        let values = try decoder.container(keyedBy: CodingKeys.self)
        id = try values.decode(String.self, forKey: .id)
        name = try values.decodeIfPresent(String.self, forKey: .name) ?? id
        description = try values.decodeIfPresent(
            String.self,
            forKey: .description
        ) ?? ""
        architecture = try values.decode(
            Architecture.self,
            forKey: .architecture)
        pricing = try values.decodeIfPresent(Pricing.self, forKey: .pricing)
        supportedParameters = try values.decodeIfPresent(
            [String].self,
            forKey: .supportedParameters
        ) ?? []
        supportedVoices = try values.decodeIfPresent(
            [String].self,
            forKey: .supportedVoices
        ) ?? []
    }

    func supports(_ purpose: ModelPurpose) -> Bool {
        let inputs = Set(architecture.inputModalities)
        let outputs = Set(architecture.outputModalities)
        switch purpose {
        case .chat:
            return inputs.contains("text") && outputs.contains("text") &&
                supportedParameters.contains("tools")
        case .transcription:
            return inputs.contains("audio") && outputs.contains("transcription")
        case .speech:
            return inputs.contains("text") && outputs.contains("speech") &&
                !supportedVoices.isEmpty
        }
    }

    func preferredVoice(
        for language: SpeechVoiceLanguage,
        preserving current: String? = nil
    ) -> String? {
        if let current, supportedVoices.contains(current) {
            return current
        }
        let preferred: [String]
        switch language {
        case .english:
            preferred = ["af_heart", "en-US-Harper:MAI-Voice-2"]
        case .french:
            preferred = ["ff_siwis", "fr-FR-Soleil:MAI-Voice-2"]
        }
        if let exact = preferred.first(where: supportedVoices.contains) {
            return exact
        }
        if let languageMatch = supportedVoices.first(where: {
            Self.voice($0, matches: language)
        }) {
            return languageMatch
        }
        return supportedVoices.first
    }

    var pricingSummary: String {
        guard let pricing else { return "Pricing not listed" }
        var parts: [String] = []
        if let prompt = Self.perMillionPrice(pricing.prompt) {
            parts.append("Input \(prompt)/M")
        }
        if let completion = Self.perMillionPrice(pricing.completion) {
            parts.append("Output \(completion)/M")
        }
        if let request = Self.unitPrice(pricing.request), request != "$0" {
            parts.append("Request \(request)")
        }
        if let image = Self.unitPrice(pricing.image), image != "$0" {
            parts.append("Image \(image)")
        }
        if let search = Self.unitPrice(pricing.webSearch), search != "$0" {
            parts.append("Search \(search)")
        }
        if parts == ["Input $0/M", "Output $0/M"] {
            return "Free"
        }
        return parts.isEmpty ? "Pricing not listed" : parts.joined(separator: " · ")
    }

    private static func voice(
        _ voice: String,
        matches language: SpeechVoiceLanguage
    ) -> Bool {
        let value = voice.lowercased()
        switch language {
        case .english:
            return value.hasPrefix("en-") || value.hasPrefix("en_") ||
                value.hasPrefix("gb_") || value.hasPrefix("af_") ||
                value.hasPrefix("am_") || value.hasPrefix("bf_") ||
                value.hasPrefix("bm_") || value.hasSuffix("-en")
        case .french:
            return value.hasPrefix("fr-") || value.hasPrefix("fr_") ||
                value.hasPrefix("ff_") || value.hasSuffix("-fr")
        }
    }

    private static func perMillionPrice(_ raw: String?) -> String? {
        guard let raw, let value = Decimal(string: raw, locale: Locale(identifier: "en_US_POSIX")) else {
            return nil
        }
        return formattedUSD(value * 1_000_000)
    }

    private static func unitPrice(_ raw: String?) -> String? {
        guard let raw, let value = Decimal(string: raw, locale: Locale(identifier: "en_US_POSIX")) else {
            return nil
        }
        return formattedUSD(value)
    }

    private static func formattedUSD(_ value: Decimal) -> String {
        let number = NSDecimalNumber(decimal: value)
        if number == .zero { return "$0" }
        let formatter = NumberFormatter()
        formatter.locale = Locale(identifier: "en_US_POSIX")
        formatter.numberStyle = .decimal
        formatter.minimumFractionDigits = 0
        formatter.maximumFractionDigits = value < 1 ? 4 : 2
        return "$\(formatter.string(from: number) ?? number.stringValue)"
    }
}

private struct OpenRouterModelsResponse: Decodable {
    let data: [OpenRouterModel]
}

enum ModelCatalogError: LocalizedError {
    case invalidResponse
    case responseTooLarge

    var errorDescription: String? {
        switch self {
        case .invalidResponse:
            return "OpenRouter did not return a valid model list."
        case .responseTooLarge:
            return "The OpenRouter model list was larger than the app limit."
        }
    }
}

struct ModelCatalogClient {
    static let maximumResponseBytes = 4 * 1_024 * 1_024

    static func catalogRequest() throws -> URLRequest {
        guard let url = URL(
            string: "https://openrouter.ai/api/v1/models?output_modalities=all"
        ) else {
            throw ModelCatalogError.invalidResponse
        }
        var request = URLRequest(url: url)
        request.timeoutInterval = 15
        request.setValue("application/json", forHTTPHeaderField: "Accept")
        return request
    }

    func fetch() async throws -> [OpenRouterModel] {
        let request = try Self.catalogRequest()

        let configuration = URLSessionConfiguration.ephemeral
        configuration.timeoutIntervalForRequest = 15
        configuration.timeoutIntervalForResource = 20
        configuration.waitsForConnectivity = false
        let session = URLSession(configuration: configuration)
        defer { session.invalidateAndCancel() }
        let (bytes, response) = try await session.bytes(for: request)
        guard let http = response as? HTTPURLResponse else {
            throw ModelCatalogError.invalidResponse
        }
        guard (200...299).contains(http.statusCode),
              http.value(forHTTPHeaderField: "Content-Type")?
                .lowercased().hasPrefix("application/json") == true else {
            throw ModelCatalogError.invalidResponse
        }
        var data = Data()
        data.reserveCapacity(min(
            http.expectedContentLength > 0
                ? Int(http.expectedContentLength)
                : 256 * 1_024,
            Self.maximumResponseBytes))
        for try await byte in bytes {
            guard data.count < Self.maximumResponseBytes else {
                throw ModelCatalogError.responseTooLarge
            }
            data.append(byte)
        }
        return try JSONDecoder().decode(OpenRouterModelsResponse.self, from: data).data
    }
}

@MainActor
final class ModelCatalog: ObservableObject {
    @Published private(set) var models: [OpenRouterModel] = []
    @Published private(set) var isLoading = false
    @Published private(set) var errorText: String?

    private let client = ModelCatalogClient()

    func load(force: Bool = false) async {
        guard force || models.isEmpty else { return }
        guard !isLoading else { return }
        isLoading = true
        errorText = nil
        do {
            models = try await client.fetch()
        } catch {
            if Task.isCancelled {
                isLoading = false
                return
            }
            errorText = (error as? LocalizedError)?.errorDescription ??
                "The app could not load the OpenRouter model list."
        }
        isLoading = false
    }
}
