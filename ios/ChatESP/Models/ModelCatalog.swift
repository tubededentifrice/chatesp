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
            return "Converts the final answer into spoken audio. Requires text input, speech output, and the English and French voices used by ChatESP."
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

    let id: String
    let name: String
    let description: String
    let architecture: Architecture
    let supportedParameters: [String]
    let supportedVoices: [String]

    private enum CodingKeys: String, CodingKey {
        case id
        case name
        case description
        case architecture
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
                supportedVoices.contains("af_heart") &&
                supportedVoices.contains("ff_siwis")
        }
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
